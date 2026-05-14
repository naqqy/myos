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

int gfx_info(naq_gfx_info_t* out) {
    return (int)syscall1(SYS64_GFX_INFO, (u64)out);
}

int gfx_clear(u32 color) {
    return (int)syscall1(SYS64_GFX_CLEAR, (u64)color);
}

int gfx_draw_rect(i32 x, i32 y, i32 w, i32 h, u32 color) {
    naq_gfx_rect_t r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.color = color;

    return (int)syscall1(SYS64_GFX_DRAW_RECT, (u64)&r);
}

int gfx_present(const void* pixels, u32 width, u32 height) {
    naq_gfx_blit_t b;

    b.dst_x = 0;
    b.dst_y = 0;
    b.width = width;
    b.height = height;
    b.pixels = pixels;

    return (int)syscall1(SYS64_GFX_PRESENT, (u64)&b);
}

int gfx_blit(i32 x, i32 y, const void* pixels, u32 width, u32 height) {
    naq_gfx_blit_t b;

    b.dst_x = x;
    b.dst_y = y;
    b.width = width;
    b.height = height;
    b.pixels = pixels;

    return (int)syscall1(SYS64_GFX_BLIT, (u64)&b);
}

int gfx_set_mode(u32 mode) {
    return (int)syscall1(SYS64_GFX_SET_MODE, mode);
}

u32 gfx_get_mode(void) {
    return (u32)syscall0(SYS64_GFX_GET_MODE);
}