#include "../include/naqos.h"

i64 syscall0(u64 num) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );

    return ret;
}

i64 syscall1(u64 num, u64 a0) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a0)
        : "memory"
    );

    return ret;
}

i64 syscall2(u64 num, u64 a0, u64 a1) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1)
        : "memory"
    );

    return ret;
}

i64 syscall3(u64 num, u64 a0, u64 a1, u64 a2) {
    i64 ret;

    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a0), "S"(a1), "d"(a2)
        : "memory"
    );

    return ret;
}

void exit(int code) {
    syscall1(SYS64_EXIT, (u64)code);

    for (;;) {
        syscall0(SYS64_YIELD);
    }
}

i64 write(int fd, const void* buf, size_t len) {
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

int readdir(const char* path, naq_dirent_t* entries, u32 max_entries) {
    return (int)syscall3(
        SYS64_READDIR,
        (u64)path,
        (u64)entries,
        (u64)max_entries
    );
}

int spawn(const char* path) {
    return (int)syscall1(SYS64_SPAWN, (u64)path);
}

i64 read_file(const char* path, void* buf, size_t buf_size) {
    naq_stat_t st;
    size_t total = 0;

    if (!path || !buf) {
        return -1;
    }

    if (stat(path, &st) < 0 || st.size > buf_size) {
        return -1;
    }

    int fd = open(path, 0);

    if (fd < 0) {
        return -1;
    }

    while (total < st.size) {
        i64 got = read(fd, (u8*)buf + total, st.size - total);

        if (got <= 0) {
            break;
        }

        total += (size_t)got;
    }

    close(fd);

    return total == st.size ? (i64)total : -1;
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
    return (int)syscall1(SYS64_GFX_SET_MODE, (u64)mode);
}

u32 gfx_get_mode(void) {
    return (u32)syscall0(SYS64_GFX_GET_MODE);
}

int win_create(i32 x, i32 y, u32 width, u32 height, const char* title) {
    naq_win_create_t req;

    req.x = x;
    req.y = y;
    req.width = width;
    req.height = height;
    req.title = title;

    return (int)syscall1(SYS64_WIN_CREATE, (u64)&req);
}

int win_create_ex(i32 x, i32 y, u32 width, u32 height, const char* title, u32 flags) {
    naq_win_create_ex_t req;

    req.x = x;
    req.y = y;
    req.width = width;
    req.height = height;
    req.title = title;
    req.flags = flags;

    return (int)syscall1(SYS64_WIN_CREATE_EX, (u64)&req);
}

int win_create_fullscreen(const char* title) {
    return win_create_ex(
        0,
        0,
        0,
        0,
        title,
        NAQ_WIN_FLAG_FULLSCREEN |
        NAQ_WIN_FLAG_BORDERLESS |
        NAQ_WIN_FLAG_GAME
    );
}

int win_create_game_window(i32 x, i32 y, u32 width, u32 height, const char* title, int fullscreen) {
    if (fullscreen) {
        return win_create_fullscreen(title);
    }

    return win_create_ex(
        x,
        y,
        width,
        height,
        title,
        NAQ_WIN_FLAG_GAME
    );
}

int win_destroy(int window_id) {
    return (int)syscall1(SYS64_WIN_DESTROY, (u64)window_id);
}

int win_present(int window_id, const void* pixels, u32 width, u32 height) {
    naq_win_present_t req;

    req.window_id = window_id;
    req.width = width;
    req.height = height;
    req.pixels = pixels;

    return (int)syscall1(SYS64_WIN_PRESENT, (u64)&req);
}

int win_set_title(int window_id, const char* title) {
    naq_win_title_t req;

    req.window_id = window_id;
    req.title = title;

    return (int)syscall1(SYS64_WIN_SET_TITLE, (u64)&req);
}

int win_get_event(int window_id, naq_win_event_t* out) {
    return (int)syscall2(SYS64_WIN_GET_EVENT, (u64)window_id, (u64)out);
}
