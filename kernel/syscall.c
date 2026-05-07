#include "syscall.h"
#include "process.h"
#include "fat32.h"
#include "usercopy.h"
#include "timer.h"

#define SYSCALL_PATH_MAX 128
#define SYSCALL_FILE_MAX 65536

static u8 syscall_file_buffer[SYSCALL_FILE_MAX];

typedef unsigned int u32;

extern void kernel_print(const char* s);
extern void kernel_put_char(char c);
extern void kernel_clear(void);
extern void kernel_set_raw_input(int enabled);
extern void kernel_begin_frame(void);
extern void kernel_end_frame(void);

extern int kernel_poll_key(void);
extern int kernel_read_key(void);

static u32 syscall_write(u32 user_s, u32 len) {
    char chunk[128];
    u32 written = 0;

    if (!user_s) {
        return 0;
    }

    if (len > 4096) {
        len = 4096;
    }

    while (written < len) {
        u32 remaining = len - written;
        u32 take = remaining;

        if (take > sizeof(chunk)) {
            take = sizeof(chunk);
        }

        if (!user_copy_from(chunk, user_s + written, take)) {
            return written;
        }

        for (u32 i = 0; i < take; i++) {
            kernel_put_char(chunk[i]);
        }

        written += take;
    }

    return written;
}

static u32 syscall_read_file(u32 user_path, u32 user_buffer, u32 max_bytes) {
    char path[SYSCALL_PATH_MAX];
    u32 file_size = 0;

    if (!user_path || !user_buffer || max_bytes == 0) {
        return (u32)-1;
    }

    if (max_bytes > SYSCALL_FILE_MAX) {
        max_bytes = SYSCALL_FILE_MAX;
    }

    if (!user_copy_string(path, user_path, sizeof(path))) {
        return (u32)-1;
    }

    if (!fat32_read_file_path(path, syscall_file_buffer, max_bytes, &file_size)) {
        return (u32)-1;
    }

    if (!user_copy_to(user_buffer, syscall_file_buffer, file_size)) {
        return (u32)-1;
    }

    return file_size;
}

static u32 syscall_mkdir(u32 user_path) {
    char path[SYSCALL_PATH_MAX];

    if (!user_path) {
        return (u32)-1;
    }

    if (!user_copy_string(path, user_path, sizeof(path))) {
        return (u32)-1;
    }

    return fat32_mkdir_path(path) ? 0 : (u32)-1;
}

static u32 syscall_rm(u32 user_path) {
    char path[SYSCALL_PATH_MAX];

    if (!user_path) {
        return (u32)-1;
    }

    if (!user_copy_string(path, user_path, sizeof(path))) {
        return (u32)-1;
    }

    return fat32_rm_path(path) ? 0 : (u32)-1;
}

static u32 syscall_rmdir(u32 user_path) {
    char path[SYSCALL_PATH_MAX];

    if (!user_path) {
        return (u32)-1;
    }

    if (!user_copy_string(path, user_path, sizeof(path))) {
        return (u32)-1;
    }

    return fat32_rmdir_path(path) ? 0 : (u32)-1;
}

u32 syscall_handler_c(u32 num, u32 a, u32 b, u32 c, u32 d, u32 e) {
    (void)c;
    (void)d;
    (void)e;

    switch (num) {
        case SYS_EXIT:
            kernel_set_raw_input(0);
            process_exit_current((int)a);
            return 0;

        case SYS_WRITE:
            return syscall_write(a, b);

        case SYS_PUTCHAR:
            kernel_put_char((char)a);
            return 1;

        case SYS_CLEAR:
            kernel_clear();
            return 0;

        case SYS_TICKS:
            return timer_get_ticks();

        case SYS_SLEEP:
            timer_sleep_ms(a);
            return 0;

        case SYS_POLL_KEY:
            return (u32)kernel_poll_key();

        case SYS_READ_KEY:
            return (u32)kernel_read_key();

        case SYS_YIELD:
            process_yield();
            return 0;

        case SYS_GETPID:
            return process_current_pid();

        case SYS_SET_RAW_INPUT:
            kernel_set_raw_input((int)a);
            return 0;

        case SYS_BEGIN_FRAME:
            kernel_begin_frame();
            return 0;

        case SYS_END_FRAME:
            kernel_end_frame();
            return 0;

        case SYS_READ_FILE:
            return syscall_read_file(a, b, c);

        case SYS_MKDIR:
            return syscall_mkdir(a);

        case SYS_RM:
            return syscall_rm(a);

        case SYS_RMDIR:
            return syscall_rmdir(a);

        default:
            kernel_print("syscall: unknown syscall\n");
            return (u32)-1;
    }
}