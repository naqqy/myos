#include "syscall64.h"

#include "timer64.h"
#include "../../k64_console.h"

#include "../../mm/usercopy64.h"

#include "../../proc/process64.h"

#include "../../debug/bootlog64.h"

#include "../../fs/vfs64.h"

static int g_syscall64_initialized = 0;

#define SYSCALL64_WRITE_CHUNK_SIZE 128u

#define SYSCALL64_MAX_PATH 128
#define SYSCALL64_FILE_TMP_SIZE 4096u

static char g_syscall64_path_buf[SYSCALL64_MAX_PATH];
static nq_u8 g_syscall64_file_tmp[SYSCALL64_FILE_TMP_SIZE];

static nq_u64 syscall64_error(nq_u64 err) {
    return 0ull - err;
}

static nq_u64 syscall64_write(const char* user_s, nq_u64 max_len) {
    char buf[SYSCALL64_WRITE_CHUNK_SIZE];
    nq_u64 written = 0;

    if (!user_s || max_len == 0) {
        return 0;
    }

    while (written < max_len) {
        nq_u64 remaining = max_len - written;
        nq_u64 chunk = SYSCALL64_WRITE_CHUNK_SIZE;

        if (chunk > remaining) {
            chunk = remaining;
        }

        if (!usercopy64_copy_from_user(
                buf,
                user_s + written,
                chunk)) {
            break;
        }

        for (nq_u64 i = 0; i < chunk; i++) {
            char c = buf[i];

            if (c == 0) {
                return written;
            }

            if (c == '\r') {
                written++;
                continue;
            }

            if (c == '\n' || c == '\t' || (c >= 32 && c <= 126)) {
                process64_write_current_len(&c, 1);
            }

            written++;
        }
    }

    return written;
}

static int syscall64_copy_user_string(
    const char* user_s,
    char* out,
    nq_u64 out_size
) {
    if (!user_s || !out || out_size == 0) {
        return 0;
    }

    for (nq_u64 i = 0; i < out_size; i++) {
        char c = 0;

        if (!usercopy64_copy_from_user(&c, user_s + i, 1)) {
            out[0] = 0;
            return 0;
        }

        out[i] = c;

        if (c == 0) {
            return 1;
        }
    }

    out[out_size - 1] = 0;
    return 0;
}

static int syscall64_validate_read_path(const char* path) {
    if (!path || path[0] != '/') {
        return 0;
    }

    for (nq_u64 i = 0; path[i]; i++) {
        char c = path[i];

        if (c < 32 || c > 126) {
            return 0;
        }

        /*
         * Simple traversal block for now.
         */
        if (c == '.' && path[i + 1] == '.') {
            return 0;
        }
    }

    return 1;
}

void syscall64_handler(syscall64_frame_t* frame) {
    if (!frame) {
        return;
    }

    nq_u64 num = frame->rax;

    switch (num) {
        case SYS64_WRITE: {
            /*
             * RDI = const char* buffer
             * RSI = max length
             */
            const char* user_buf = (const char*)frame->rdi;
            nq_u64 len = frame->rsi;

            if (!user_buf || len == 0) {
                frame->rax = 0;
                break;
            }

            frame->rax = syscall64_write(user_buf, len);
            break;
        }

        case SYS64_EXIT: {
            /*
             * RDI = exit code
             */
            if (frame->rdi != 0) {
                k64_print("\n[user] app exited with code=");
                k64_print_dec64(frame->rdi);
                k64_print("\n");
            }

            process64_exit();

            frame->rax = 0;
            break;

            process64_exit();

            /*
             * Fallback if process64_exit ever returns.
             */
            frame->rax = 0;
            break;
        }

        case SYS64_GET_TICKS:
            frame->rax = timer64_get_ticks();
            break;

        case SYS64_YIELD:
            process64_yield();
            frame->rax = 0;
            break;

        case SYS64_GETPID:
            frame->rax = process64_current_pid();
            break;

        case SYS64_SLEEP: {
            /*
             * RDI = ticks to sleep
             */
            nq_u64 ticks = frame->rdi;

            /*
             * POC guard so a bad app cannot ask for a massive spin-yield sleep.
             * 100000 ticks is arbitrary; adjust if your timer frequency is different.
             */
            if (ticks > 100000ull) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            process64_sleep_ticks(ticks);
            frame->rax = 0;
            break;
        }

        case SYS64_INPUT_POLL_KEY:
            frame->rax = (nq_u64)process64_input_poll_key();
            break;

        case SYS64_INPUT_GET_KEY:
            frame->rax = (nq_u64)process64_input_get_key();
            break;

        case SYS64_OPEN: {
            /*
            * RDI = const char* path
            * RSI = flags, must be 0 for read-only
            */
            const char* user_path = (const char*)frame->rdi;
            nq_u64 flags = frame->rsi;

            if (flags != 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            if (!syscall64_copy_user_string(
                    user_path,
                    g_syscall64_path_buf,
                    sizeof(g_syscall64_path_buf))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!syscall64_validate_read_path(g_syscall64_path_buf)) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            nq_u64 size = 0;

            /*
            * POC existence check. This reads up to tmp size.
            */
            if (!vfs64_read_file(
                    g_syscall64_path_buf,
                    g_syscall64_file_tmp,
                    SYSCALL64_FILE_TMP_SIZE,
                    &size)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            int handle = process64_file_open(g_syscall64_path_buf, size);

            if (handle < 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = (nq_u64)handle;
            break;
        }

        case SYS64_READ: {
            /*
            * RDI = handle
            * RSI = user buffer
            * RDX = max bytes
            */
            int handle = (int)frame->rdi;
            char* user_buf = (char*)frame->rsi;
            nq_u64 max_bytes = frame->rdx;

            if (!user_buf || max_bytes == 0) {
                frame->rax = 0;
                break;
            }

            if (max_bytes > SYSCALL64_FILE_TMP_SIZE) {
                max_bytes = SYSCALL64_FILE_TMP_SIZE;
            }

            process64_file_t* file = process64_file_from_handle(handle);

            if (!file) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            nq_u64 file_size = 0;

            if (!vfs64_read_file(
                    file->path,
                    g_syscall64_file_tmp,
                    SYSCALL64_FILE_TMP_SIZE,
                    &file_size)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (file->offset >= file_size) {
                frame->rax = 0;
                break;
            }

            nq_u64 available = file_size - file->offset;
            nq_u64 to_copy = max_bytes;

            if (to_copy > available) {
                to_copy = available;
            }

            /*
            * Current POC limitation:
            * g_syscall64_file_tmp only contains first SYSCALL64_FILE_TMP_SIZE bytes.
            */
            if (file->offset + to_copy > SYSCALL64_FILE_TMP_SIZE) {
                to_copy = SYSCALL64_FILE_TMP_SIZE - file->offset;
            }

            if (to_copy == 0) {
                frame->rax = 0;
                break;
            }

            if (!usercopy64_copy_to_user(
                    user_buf,
                    g_syscall64_file_tmp + file->offset,
                    to_copy)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            file->offset += to_copy;
            frame->rax = to_copy;
            break;
        }

        case SYS64_CLOSE: {
            int handle = (int)frame->rdi;

            if (process64_file_close(handle) < 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = 0;
            break;
        }

        case SYS64_STAT: {
            /*
            * RDI = const char* path
            * RSI = user naq_stat_t*
            */
            const char* user_path = (const char*)frame->rdi;
            naq_stat_t* user_stat = (naq_stat_t*)frame->rsi;

            if (!user_stat) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!syscall64_copy_user_string(
                    user_path,
                    g_syscall64_path_buf,
                    sizeof(g_syscall64_path_buf))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!syscall64_validate_read_path(g_syscall64_path_buf)) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            nq_u64 size = 0;

            if (!vfs64_read_file(
                    g_syscall64_path_buf,
                    g_syscall64_file_tmp,
                    SYSCALL64_FILE_TMP_SIZE,
                    &size)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            naq_stat_t st;
            st.size = size;
            st.type = NAQ_STAT_TYPE_FILE;
            st.reserved = 0;

            if (!usercopy64_copy_to_user(user_stat, &st, sizeof(st))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            frame->rax = 0;
            break;
        }

        default:
            k64_print("\n[user] unknown syscall: ");
            k64_print_dec64(num);
            k64_print("\n");
            frame->rax = syscall64_error(SYS64_ENOSYS);
            break;
    }
}

void syscall64_init(void) {
    g_syscall64_initialized = 1;
    bootlog64_print("Syscall64 ABI initialized: int 0x80\n");
}