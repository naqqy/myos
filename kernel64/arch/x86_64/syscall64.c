#include "syscall64.h"

#include "timer64.h"
#include "../../k64_console.h"

#include "../../mm/usercopy64.h"

#include "../../proc/process64.h"

#include "../../debug/bootlog64.h"

static int g_syscall64_initialized = 0;

#define SYSCALL64_WRITE_CHUNK_SIZE 128u

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

void syscall64_handler(syscall64_frame_t* frame) {
    if (!frame) {
        return;
    }

    nq_u64 num = frame->rax;

    switch (num) {
        case SYS64_WRITE:
            /*
             * rdi = const char* buffer
             * rsi = max length
             */
            frame->rax = syscall64_write(
                (const char*)frame->rdi,
                frame->rsi
            );
            break;

        case SYS64_EXIT:
            k64_print("\n[user] SYS_EXIT code=");
            k64_print_dec64(frame->rdi);
            k64_print("\n");

            /*
            * This should not return for a normal process.
            * It marks the current process ZOMBIE and switches away.
            */
            process64_exit();

            /*
            * Fallback if process64_exit ever returns.
            */
            frame->rax = 0;
            break;

        case SYS64_GET_TICKS:
            frame->rax = timer64_get_ticks();
            break;

        default:
            k64_print("\n[user] unknown syscall: ");
            k64_print_dec64(num);
            k64_print("\n");
            frame->rax = (nq_u64)-1;
            break;
    }
}

void syscall64_init(void) {
    g_syscall64_initialized = 1;
    bootlog64_print("Syscall64 ABI initialized: int 0x80\n");
}