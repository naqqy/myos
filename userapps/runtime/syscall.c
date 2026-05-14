#include "../include/naqos.h"

i64 syscall0(u64 num) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );

    return ret;
}

i64 syscall1(u64 num, u64 a0) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a0)
        : "rcx", "r11", "memory"
    );

    return ret;
}

i64 syscall2(u64 num, u64 a0, u64 a1) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory"
    );

    return ret;
}

i64 syscall3(u64 num, u64 a0, u64 a1, u64 a2) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );

    return ret;
}

void exit(int code) {
    syscall1(SYS64_EXIT, (u64)code);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

i64 write(int fd, const void* buf, size_t len) {
    /*
     * Current kernel SYS_WRITE ABI:
     *   RAX = SYS64_WRITE
     *   RDI = user buffer
     *   RSI = length
     *
     * fd is ignored for now.
     */
    (void)fd;

    return syscall2(SYS64_WRITE, (u64)buf, (u64)len);
}

u64 get_ticks(void) {
    return (u64)syscall0(SYS64_GET_TICKS);
}

void yield(void) {
    syscall0(SYS64_YIELD);
}

u64 getpid(void) {
    return (u64)syscall0(SYS64_GETPID);
}

i64 sleep_ticks(u64 ticks) {
    return syscall1(SYS64_SLEEP, ticks);
}

void putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
}

void puts(const char* s) {
    if (!s) {
        return;
    }

    write(1, s, strlen(s));
}

int input_poll_key(void) {
    return (int)syscall0(SYS64_INPUT_POLL_KEY);
}

int input_get_key(void) {
    return (int)syscall0(SYS64_INPUT_GET_KEY);
}

int open(const char* path, u64 flags) {
    return (int)syscall2(SYS64_OPEN, (u64)path, flags);
}

i64 read(int fd, void* buf, size_t len) {
    return syscall3(SYS64_READ, (u64)fd, (u64)buf, (u64)len);
}

int close(int fd) {
    return (int)syscall1(SYS64_CLOSE, (u64)fd);
}

int stat(const char* path, naq_stat_t* out) {
    return (int)syscall2(SYS64_STAT, (u64)path, (u64)out);
}