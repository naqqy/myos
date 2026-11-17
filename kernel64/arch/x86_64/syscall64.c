#include "syscall64.h"

#include "timer64.h"
#include "../../k64_console.h"

#include "../../mm/usercopy64.h"

#include "../../proc/process64.h"

#include "../../debug/bootlog64.h"

#include "../../fs/vfs64.h"

#include "../../graphics/framebuffer64.h"

#include "../../gui/gui64.h"

#include "../../mm/kmem64.h"

#include "../../exec/elf64.h"

static int g_syscall64_initialized = 0;

#define SYSCALL64_WRITE_CHUNK_SIZE 128u

#define SYSCALL64_MAX_PATH 128
/*
 * Largest file an app may open. The whole file is held in kernel heap
 * memory while the handle is open.
 */
#define SYSCALL64_MAX_FILE_SIZE (64ull * 1024ull * 1024ull)

/*
 * Largest window surface an app may present. The pixels are copied straight
 * from validated user memory into the window buffer, so no kernel-side
 * temporary is needed (the old 33 MiB static temp pushed kernel BSS past the
 * memory the UEFI loader reserves and into the user virtual range).
 */
#define SYSCALL64_WIN_MAX_PIXELS (3840u * 2160u)

static char g_syscall64_path_buf[SYSCALL64_MAX_PATH];

typedef struct syscall64_gfx_info {
    nq_u32 width;
    nq_u32 height;
    nq_u32 pitch;
    nq_u32 format;
} syscall64_gfx_info_t;

typedef struct syscall64_gfx_rect {
    nq_s32 x;
    nq_s32 y;
    nq_s32 w;
    nq_s32 h;
    nq_u32 color;
} syscall64_gfx_rect_t;

typedef struct syscall64_gfx_blit {
    nq_s32 dst_x;
    nq_s32 dst_y;
    nq_u32 width;
    nq_u32 height;
    const void* pixels;
} syscall64_gfx_blit_t;

typedef struct syscall64_win_create {
    int x;
    int y;
    nq_u32 width;
    nq_u32 height;
    const char* title;
} syscall64_win_create_t;

typedef struct syscall64_win_create_ex {
    int x;
    int y;
    nq_u32 width;
    nq_u32 height;
    const char* title;
    nq_u32 flags;
} syscall64_win_create_ex_t;

typedef struct syscall64_win_present {
    int window_id;
    nq_u32 width;
    nq_u32 height;
    const void* pixels;
} syscall64_win_present_t;

typedef struct syscall64_win_title {
    int window_id;
    const char* title;
} syscall64_win_title_t;

typedef struct syscall64_win_event {
    nq_u32 type;
    nq_u32 key;
    int x;
    int y;
    int w;
    int h;
} syscall64_win_event_t;

#define SYSCALL64_GFX_MAX_BLIT_PIXELS (320u * 240u)

static nq_u32 g_syscall64_gfx_tmp[SYSCALL64_GFX_MAX_BLIT_PIXELS];

static nq_u64 syscall64_error(nq_u64 err) {
    return 0ull - err;
}

static nq_u32 syscall64_gfx_color_to_native(nq_u32 color) {
    nq_u8 r = (nq_u8)((color >> 16) & 0xFFu);
    nq_u8 g = (nq_u8)((color >> 8) & 0xFFu);
    nq_u8 b = (nq_u8)(color & 0xFFu);

    return fb64_make_color(r, g, b);
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

#define SYSCALL64_READDIR_MAX 512u

typedef struct syscall64_readdir_ctx {
    naq_dirent_t* entries;
    nq_u32 capacity;
    nq_u32 count;
} syscall64_readdir_ctx_t;

static void syscall64_readdir_collect(
    const char* name,
    nq_u64 size,
    int is_dir,
    void* ctx_ptr
) {
    syscall64_readdir_ctx_t* ctx = (syscall64_readdir_ctx_t*)ctx_ptr;

    if (!name || ctx->count >= ctx->capacity) {
        return;
    }

    /*
     * "." and ".." are not useful to apps; they navigate with paths.
     */
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
        return;
    }

    naq_dirent_t* e = &ctx->entries[ctx->count++];
    nq_u32 i = 0;

    while (name[i] && i + 1 < NAQ_DIRENT_NAME_MAX) {
        e->name[i] = name[i];
        i++;
    }

    while (i < NAQ_DIRENT_NAME_MAX) {
        e->name[i++] = 0;
    }

    e->type = is_dir ? NAQ_STAT_TYPE_DIR : NAQ_STAT_TYPE_FILE;
    e->size = size;
}

static int syscall64_should_guard_preemption(nq_u64 num) {
    /*
     * Do not guard syscalls whose purpose is to block/switch/terminate.
     * Everything else is protected from timer-based task switching while
     * using shared kernel syscall buffers.
     *
     * SYS64_INPUT_GET_KEY blocks until a key arrives. If it held the guard,
     * no other task could run while an app waits for input.
     */
    switch (num) {
        case SYS64_EXIT:
        case SYS64_YIELD:
        case SYS64_SLEEP:
        case SYS64_INPUT_GET_KEY:
            return 0;

        default:
            return 1;
    }
}


void syscall64_handler(syscall64_frame_t* frame) {
    if (!frame) {
        return;
    }

    nq_u64 num = frame->rax;
    int preempt_guard = syscall64_should_guard_preemption(num);

    if (preempt_guard) {
        process64_preempt_disable();
    }

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
            void* data = 0;

            /*
             * Read the whole file once into a heap buffer owned by the
             * handle. SYS64_READ then copies out of it at the handle's
             * offset, so files are not limited to a fixed temp buffer and
             * are not re-read from the filesystem on every call.
             */
            if (!vfs64_read_file_alloc(
                    g_syscall64_path_buf,
                    SYSCALL64_MAX_FILE_SIZE,
                    &data,
                    &size)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            int handle = process64_file_open(
                g_syscall64_path_buf,
                (nq_u8*)data,
                size
            );

            if (handle < 0) {
                kfree64(data);
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

            process64_file_t* file = process64_file_from_handle(handle);

            if (!file || !file->data) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            if (file->offset >= file->size) {
                frame->rax = 0;
                break;
            }

            nq_u64 available = file->size - file->offset;
            nq_u64 to_copy = max_bytes;

            if (to_copy > available) {
                to_copy = available;
            }

            if (!usercopy64_copy_to_user(
                    user_buf,
                    file->data + file->offset,
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

            vfs64_stat_t vst;

            if (!vfs64_stat(g_syscall64_path_buf, &vst)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            naq_stat_t st;
            st.size = vst.size;
            st.type = vst.type == VFS64_TYPE_DIR ?
                NAQ_STAT_TYPE_DIR :
                NAQ_STAT_TYPE_FILE;
            st.reserved = 0;

            if (!usercopy64_copy_to_user(user_stat, &st, sizeof(st))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            frame->rax = 0;
            break;
        }

        case SYS64_GFX_INFO: {
            syscall64_gfx_info_t info;
            syscall64_gfx_info_t* user_info =
                (syscall64_gfx_info_t*)frame->rdi;

            if (!user_info) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            info.width = fb64_width();
            info.height = fb64_height();
            info.pitch = fb64_pitch();
            info.format = fb64_format();

            if (!usercopy64_copy_to_user(user_info, &info, sizeof(info))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            frame->rax = 0;
            break;
        }

        case SYS64_GFX_CLEAR: {
            nq_u32 color = (nq_u32)frame->rdi;

            /*
             * If this is a graphical app running in the background,
             * do not touch the real framebuffer.
             *
             * For now, graphical apps should clear their own user backbuffer
             * with gfx_buffer_clear().
             */
            if (process64_get_graphics_mode() == PROCESS64_GFX_ACTIVE &&
                !process64_current_is_foreground()) {
                frame->rax = 0;
                break;
            }

            fb64_clear(syscall64_gfx_color_to_native(color));

            frame->rax = 0;
            break;
        }

        case SYS64_GFX_DRAW_RECT: {
            syscall64_gfx_rect_t r;

            if (!frame->rdi) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!usercopy64_copy_from_user(
                    &r,
                    (const void*)frame->rdi,
                    sizeof(r))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            /*
             * Background graphical apps should not touch the real framebuffer.
             * They should draw into their own user backbuffer and present it.
             */
            if (process64_get_graphics_mode() == PROCESS64_GFX_ACTIVE &&
                !process64_current_is_foreground()) {
                frame->rax = 0;
                break;
            }

            fb64_fill_rect(
                (nq_u32)r.x,
                (nq_u32)r.y,
                (nq_u32)r.w,
                (nq_u32)r.h,
                syscall64_gfx_color_to_native(r.color)
            );

            frame->rax = 0;
            break;
        }

        case SYS64_GFX_PRESENT:
        case SYS64_GFX_BLIT: {
            syscall64_gfx_blit_t b;
            nq_u64 pixel_count;
            nq_u64 byte_count;
            int is_graphics_app = 0;
            int is_foreground = 0;

            if (!frame->rdi) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!usercopy64_copy_from_user(
                    &b,
                    (const void*)frame->rdi,
                    sizeof(b))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!b.pixels || b.width == 0 || b.height == 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            pixel_count = (nq_u64)b.width * (nq_u64)b.height;

            if (pixel_count > SYSCALL64_GFX_MAX_BLIT_PIXELS) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            byte_count = pixel_count * sizeof(nq_u32);

            if (!usercopy64_copy_from_user(
                    g_syscall64_gfx_tmp,
                    b.pixels,
                    byte_count)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (num == SYS64_GFX_PRESENT) {
                b.dst_x = 0;
                b.dst_y = 0;
            }

            /*
             * If this process requested graphical app mode, store the
             * latest frame in the process-owned graphics buffer.
             *
             * If it is also foreground, draw it to the real framebuffer.
             * If it is background, only remember the frame so view <pid>
             * can show it later.
             */
            is_graphics_app =
                process64_get_graphics_mode() == PROCESS64_GFX_ACTIVE;

            is_foreground = process64_current_is_foreground();

            if (is_graphics_app) {
                if (!process64_present_current_gfx(
                        g_syscall64_gfx_tmp,
                        b.width,
                        b.height,
                        b.dst_x,
                        b.dst_y)) {
                    frame->rax = syscall64_error(SYS64_EINVAL);
                    break;
                }

                if (is_foreground) {
                    fb64_blit_rgb32(
                        b.dst_x,
                        b.dst_y,
                        b.width,
                        b.height,
                        g_syscall64_gfx_tmp
                    );
                }

                frame->rax = 0;
                break;
            }

            /*
             * Non-graphical legacy/direct-screen path.
             */
            fb64_blit_rgb32(
                b.dst_x,
                b.dst_y,
                b.width,
                b.height,
                g_syscall64_gfx_tmp
            );

            frame->rax = 0;
            break;
        }

        case SYS64_GFX_SET_MODE: {
            nq_u32 mode = (nq_u32)frame->rdi;

            if (mode != 0 && mode != 1) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            if (!process64_set_graphics_mode(mode)) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = 0;
            break;
        }

        case SYS64_GFX_GET_MODE:
            frame->rax = process64_get_graphics_mode();
            break;

        case SYS64_WIN_CREATE: {
            syscall64_win_create_t req;
            char title[32];

            if (!frame->rdi) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!usercopy64_copy_from_user(
                    &req,
                    (const void*)frame->rdi,
                    sizeof(req))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (req.width == 0 || req.height == 0 ||
                req.width > 1024 || req.height > 768) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            title[0] = 0;

            if (req.title) {
                if (!syscall64_copy_user_string(req.title, title, sizeof(title))) {
                    frame->rax = syscall64_error(SYS64_EFAULT);
                    break;
                }
            } else {
                title[0] = 'A';
                title[1] = 'p';
                title[2] = 'p';
                title[3] = 0;
            }

            int id = gui64_create_app_window_ex(
                (int)process64_current_pid(),
                title,
                req.x,
                req.y,
                (int)req.width,
                (int)req.height,
                GUI64_WIN_FLAG_BORDERED
            );

            if (id < 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = (nq_u64)id;
            break;
        }

        case SYS64_WIN_CREATE_EX: {
            syscall64_win_create_ex_t req;
            char title[32];

            if (!frame->rdi) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!usercopy64_copy_from_user(
                    &req,
                    (const void*)frame->rdi,
                    sizeof(req))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (req.flags & ~(GUI64_WIN_FLAG_BORDERLESS |
                              GUI64_WIN_FLAG_FULLSCREEN |
                              GUI64_WIN_FLAG_GAME)) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            if (!(req.flags & GUI64_WIN_FLAG_FULLSCREEN)) {
                if (req.width == 0 || req.height == 0 ||
                    req.width > 1024 || req.height > 768) {
                    frame->rax = syscall64_error(SYS64_EINVAL);
                    break;
                }
            }

            title[0] = 0;

            if (req.title) {
                if (!syscall64_copy_user_string(req.title, title, sizeof(title))) {
                    frame->rax = syscall64_error(SYS64_EFAULT);
                    break;
                }
            } else {
                title[0] = 'A';
                title[1] = 'p';
                title[2] = 'p';
                title[3] = 0;
            }

            int id = gui64_create_app_window_ex(
                (int)process64_current_pid(),
                title,
                req.x,
                req.y,
                (int)req.width,
                (int)req.height,
                req.flags
            );

            if (id < 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = (nq_u64)id;
            break;
        }

        case SYS64_WIN_PRESENT: {
            syscall64_win_present_t req;

            if (!frame->rdi) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!usercopy64_copy_from_user(
                    &req,
                    (const void*)frame->rdi,
                    sizeof(req))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!req.pixels || req.width == 0 || req.height == 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            nq_u64 count = (nq_u64)req.width * (nq_u64)req.height;

            if (count > SYSCALL64_WIN_MAX_PIXELS) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            nq_u64 bytes = count * sizeof(nq_u32);

            /*
             * Validate the whole user pixel range, then let the compositor
             * copy directly from user memory. We are still on this process's
             * address space and the syscall preemption guard is held.
             */
            if (!usercopy64_check_read(req.pixels, bytes)) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!gui64_present_window(
                    req.window_id,
                    (int)process64_current_pid(),
                    (const nq_u32*)req.pixels,
                    req.width,
                    req.height)) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = 0;
            break;
        }

        case SYS64_WIN_DESTROY: {
            int id = (int)frame->rdi;

            if (!gui64_destroy_window(id, (int)process64_current_pid())) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = 0;
            break;
        }

        case SYS64_WIN_SET_TITLE: {
            syscall64_win_title_t req;
            char title[32];

            if (!frame->rdi) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!usercopy64_copy_from_user(
                    &req,
                    (const void*)frame->rdi,
                    sizeof(req))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!req.title) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            if (!syscall64_copy_user_string(req.title, title, sizeof(title))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!gui64_set_window_title(
                    req.window_id,
                    (int)process64_current_pid(),
                    title)) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = 0;
            break;
        }

        case SYS64_WIN_GET_EVENT: {
            int id = (int)frame->rdi;
            syscall64_win_event_t* user_event =
                (syscall64_win_event_t*)frame->rsi;

            if (!user_event) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!gui64_window_belongs_to(id, (int)process64_current_pid())) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            gui64_event_t kev;
            syscall64_win_event_t ev;

            int has_event = gui64_pop_event(
                id,
                (int)process64_current_pid(),
                &kev
            );

            ev.type = kev.type;
            ev.key = kev.key;
            ev.x = kev.x;
            ev.y = kev.y;
            ev.w = kev.w;
            ev.h = kev.h;

            if (!has_event) {
                ev.type = 0;
                ev.key = 0;
                ev.x = 0;
                ev.y = 0;
                ev.w = 0;
                ev.h = 0;
            }

            if (!usercopy64_copy_to_user(user_event, &ev, sizeof(ev))) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            frame->rax = has_event ? 1u : 0u;
            break;
        }


        case SYS64_READDIR: {
            /*
             * RDI = const char* path
             * RSI = user naq_dirent_t array
             * RDX = number of entries the array can hold
             * Returns the number of entries written.
             */
            const char* user_path = (const char*)frame->rdi;
            naq_dirent_t* user_entries = (naq_dirent_t*)frame->rsi;
            nq_u64 capacity = frame->rdx;

            if (!user_entries || capacity == 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            if (capacity > SYSCALL64_READDIR_MAX) {
                capacity = SYSCALL64_READDIR_MAX;
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

            syscall64_readdir_ctx_t ctx;

            ctx.capacity = (nq_u32)capacity;
            ctx.count = 0;
            ctx.entries =
                (naq_dirent_t*)kmalloc64(capacity * sizeof(naq_dirent_t));

            if (!ctx.entries) {
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            if (!vfs64_enum_dir(
                    g_syscall64_path_buf,
                    syscall64_readdir_collect,
                    &ctx)) {
                kfree64(ctx.entries);
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            if (ctx.count > 0 &&
                !usercopy64_copy_to_user(
                    user_entries,
                    ctx.entries,
                    (nq_u64)ctx.count * sizeof(naq_dirent_t))) {
                kfree64(ctx.entries);
                frame->rax = syscall64_error(SYS64_EFAULT);
                break;
            }

            kfree64(ctx.entries);
            frame->rax = ctx.count;
            break;
        }

        case SYS64_SPAWN: {
            /*
             * RDI = const char* path of an ELF program
             * Returns the new process id.
             */
            const char* user_path = (const char*)frame->rdi;

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

            int pid = elf64_spawn_file_ex(
                g_syscall64_path_buf,
                PROCESS64_SPAWN_PRIVATE_TERMINAL
            );

            if (pid < 0) {
                frame->rax = syscall64_error(SYS64_EINVAL);
                break;
            }

            frame->rax = (nq_u64)pid;
            break;
        }

        default:
            k64_print("\n[user] unknown syscall: ");
            k64_print_dec64(num);
            k64_print("\n");
            frame->rax = syscall64_error(SYS64_ENOSYS);
            break;
    }

    if (preempt_guard) {
        process64_preempt_enable();
    }
}

void syscall64_init(void) {
    g_syscall64_initialized = 1;
    bootlog64_print("Syscall64 ABI initialized: int 0x80\n");
}