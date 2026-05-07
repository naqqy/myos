#ifndef APP_SYSCALL_H
#define APP_SYSCALL_H

typedef unsigned int u32;

/*
 * LEGACY / REMOVED KERNEL API
 *
 * This old app model passed a kernel_api_t* directly into app_main().
 * It has been phased out in favor of ring-3 apps using int 0x80 syscalls.
 *
 * Keep this note temporarily in case compatibility issues appear while
 * converting old apps.
 *
 * Old fields included:
 *   read_line
 *   reboot
 *   shutdown
 *   ls
 *   run
 * 
 * all old kernel api calls were:
 * 
 * typedef struct kernel_api {
 *   void (*print)(const char* s);
 *   void (*put_char)(char c);
 *   void (*clear)(void);
 *   int  (*read_line)(char* buf, u32 max_len);
 *   void (*reboot)(void);
 *   void (*shutdown)(void);
 *   void (*ls)(void);
 *   int (*cat)(const char* filename);
 *   int (*run)(const char* filename);
 *   u32  (*ticks)(void);
 *   void (*sleep)(u32 milliseconds);
 *   u32  (*current_pid)(void);
 *   void (*print_dec32)(u32 value);
 *   void (*yield)(void);
 *
 *   int (*poll_key)(void);
 *   int (*read_key)(void);
 *   void (*set_raw_input)(int enabled);
 *
 *   void (*begin_frame)(void);
 *   void (*end_frame)(void);
 * } kernel_api_t;
 * 
 */

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_PUTCHAR         2
#define SYS_CLEAR           3
#define SYS_TICKS           4
#define SYS_SLEEP           5
#define SYS_POLL_KEY        6
#define SYS_READ_KEY        7
#define SYS_YIELD           8
#define SYS_GETPID          9
#define SYS_SET_RAW_INPUT   10
#define SYS_BEGIN_FRAME     11
#define SYS_END_FRAME       12
#define SYS_READ_FILE       13
#define SYS_MKDIR           14
#define SYS_RM              15
#define SYS_RMDIR           16

static inline u32 syscall0(u32 num) {
    u32 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "ebx", "ecx", "edx", "esi", "edi", "memory"
    );

    return ret;
}

static inline u32 syscall1(u32 num, u32 a) {
    u32 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a)
        : "ecx", "edx", "esi", "edi", "memory"
    );

    return ret;
}

static inline u32 syscall2(u32 num, u32 a, u32 b) {
    u32 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b)
        : "edx", "esi", "edi", "memory"
    );

    return ret;
}

static inline u32 syscall3(u32 num, u32 a, u32 b, u32 c) {
    u32 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
        : "esi", "edi", "memory"
    );

    return ret;
}

static inline u32 app_strlen(const char* s) {
    u32 n = 0;

    if (!s) {
        return 0;
    }

    while (s[n]) {
        n++;
    }

    return n;
}

static inline void app_exit(int code) {
    syscall1(SYS_EXIT, (u32)code);

    for (;;) {
    }
}

static inline u32 app_write(const char* s) {
    return syscall2(SYS_WRITE, (u32)s, app_strlen(s));
}

static inline void app_putchar(char c) {
    syscall1(SYS_PUTCHAR, (u32)c);
}

static inline void app_clear(void) {
    syscall0(SYS_CLEAR);
}

static inline u32 app_ticks(void) {
    return syscall0(SYS_TICKS);
}

static inline void app_sleep(u32 ms) {
    syscall1(SYS_SLEEP, ms);
}

static inline int app_poll_key(void) {
    return (int)syscall0(SYS_POLL_KEY);
}

static inline int app_read_key(void) {
    return (int)syscall0(SYS_READ_KEY);
}

static inline void app_yield(void) {
    syscall0(SYS_YIELD);
}

static inline u32 app_getpid(void) {
    return syscall0(SYS_GETPID);
}

static inline void app_print_dec32(u32 value) {
    char buf[16];
    u32 i = 0;

    if (value == 0) {
        app_putchar('0');
        return;
    }

    while (value > 0 && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        app_putchar(buf[--i]);
    }
}

static inline void app_set_raw_input(int enabled) {
    syscall1(SYS_SET_RAW_INPUT, enabled ? 1u : 0u);
}

static inline void app_begin_frame(void) {
    syscall0(SYS_BEGIN_FRAME);
}

static inline void app_end_frame(void) {
    syscall0(SYS_END_FRAME);
}

static inline int app_read_file(const char* path, void* buffer, u32 max_bytes) {
    return (int)syscall3(SYS_READ_FILE, (u32)path, (u32)buffer, max_bytes);
}

static inline int app_mkdir(const char* path) {
    return (int)syscall1(SYS_MKDIR, (u32)path);
}

static inline int app_rm(const char* path) {
    return (int)syscall1(SYS_RM, (u32)path);
}

static inline int app_rmdir(const char* path) {
    return (int)syscall1(SYS_RMDIR, (u32)path);
}

#endif