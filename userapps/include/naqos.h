#ifndef NAQOS_H
#define NAQOS_H

typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

typedef u64 size_t;

#define NAQ_TIMER_HZ 1000ull

/*
 * Syscall IDs.
 */
#define SYS64_WRITE           0
#define SYS64_EXIT            1
#define SYS64_GET_TICKS       2
#define SYS64_YIELD           3
#define SYS64_GETPID          4
#define SYS64_SLEEP           5
#define SYS64_INPUT_POLL_KEY  6
#define SYS64_INPUT_GET_KEY   7
#define SYS64_OPEN            8
#define SYS64_READ            9
#define SYS64_CLOSE           10
#define SYS64_STAT            11
#define SYS64_GFX_INFO        12
#define SYS64_GFX_CLEAR       13
#define SYS64_GFX_PRESENT     14
#define SYS64_GFX_DRAW_RECT   15
#define SYS64_GFX_BLIT        16
#define SYS64_GFX_SET_MODE    17
#define SYS64_GFX_GET_MODE    18

/*
 * File/stat API.
 */
#define NAQ_STAT_TYPE_UNKNOWN 0
#define NAQ_STAT_TYPE_FILE    1
#define NAQ_STAT_TYPE_DIR     2

typedef struct naq_stat {
    u64 size;
    u32 type;
    u32 reserved;
} naq_stat_t;

/*
 * Graphics API.
 *
 * App colors/pixels use 0x00RRGGBB.
 * The kernel converts to the native framebuffer format.
 */
#define GFX_MODE_TEXT      0
#define GFX_MODE_GRAPHICS  1

typedef struct naq_gfx_info {
    u32 width;
    u32 height;
    u32 pitch;
    u32 format;
} naq_gfx_info_t;

typedef struct naq_gfx_rect {
    i32 x;
    i32 y;
    i32 w;
    i32 h;
    u32 color;
} naq_gfx_rect_t;

typedef struct naq_gfx_blit {
    i32 dst_x;
    i32 dst_y;
    u32 width;
    u32 height;
    const void* pixels;
} naq_gfx_blit_t;

typedef struct naq_gfx_buffer {
    u32* pixels;
    u32 width;
    u32 height;
    u32 stride;

    /*
     * Dirty rectangle is half-open:
     *   [dirty_x0, dirty_x1)
     *   [dirty_y0, dirty_y1)
     *
     * Empty when dirty_x1 <= dirty_x0 or dirty_y1 <= dirty_y0.
     */
    i32 dirty_x0;
    i32 dirty_y0;
    i32 dirty_x1;
    i32 dirty_y1;
} naq_gfx_buffer_t;

/*
 * Raw syscall wrappers.
 */
i64 syscall0(u64 num);
i64 syscall1(u64 num, u64 a0);
i64 syscall2(u64 num, u64 a0, u64 a1);
i64 syscall3(u64 num, u64 a0, u64 a1, u64 a2);

/*
 * Process/runtime API.
 */
void exit(int code);
i64 write(int fd, const void* buf, size_t len);
u64 get_ticks(void);
void yield(void);
u64 getpid(void);
i64 sleep_ticks(u64 ticks);

/*
 * Tiny libc-style helpers.
 */
size_t strlen(const char* s);
void* memset(void* dst, int value, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

void putchar(int c);
void puts(const char* s);

/*
 * Timing/frame helper API.
 */
typedef struct naq_frame_timer {
    u64 target_fps;
    u64 frame_ticks;
    u64 last_tick;
    u64 frame_start;
    u64 frame_end;
    u64 frame_delta;
    u64 frame_count;
} naq_frame_timer_t;

u64 time_now_ticks(void);
u64 time_ticks_per_second(void);
u64 time_ms_to_ticks(u64 ms);
u64 time_ticks_to_ms(u64 ticks);
void sleep_ms(u64 ms);

void frame_timer_init(naq_frame_timer_t* timer, u64 target_fps);
void frame_timer_begin(naq_frame_timer_t* timer);
void frame_timer_end(naq_frame_timer_t* timer);
u64 frame_timer_delta_ticks(const naq_frame_timer_t* timer);
u64 frame_timer_delta_ms(const naq_frame_timer_t* timer);

/*
 * Input API.
 */
int input_poll_key(void);
int input_get_key(void);

/*
 * Read-only file API.
 */
int open(const char* path, u64 flags);
i64 read(int fd, void* buf, size_t len);
int close(int fd);
int stat(const char* path, naq_stat_t* out);

/*
 * Direct graphics syscalls.
 */
int gfx_info(naq_gfx_info_t* out);
int gfx_clear(u32 color);
int gfx_draw_rect(i32 x, i32 y, i32 w, i32 h, u32 color);
int gfx_present(const void* pixels, u32 width, u32 height);
int gfx_blit(i32 x, i32 y, const void* pixels, u32 width, u32 height);
int gfx_set_mode(u32 mode);
u32 gfx_get_mode(void);

/*
 * App-owned backbuffer helpers.
 */
void gfx_buffer_init(
    naq_gfx_buffer_t* buffer,
    u32* pixels,
    u32 width,
    u32 height
);

void gfx_buffer_clear(
    naq_gfx_buffer_t* buffer,
    u32 color
);

void gfx_buffer_put_pixel(
    naq_gfx_buffer_t* buffer,
    i32 x,
    i32 y,
    u32 color
);

void gfx_buffer_draw_rect(
    naq_gfx_buffer_t* buffer,
    i32 x,
    i32 y,
    i32 w,
    i32 h,
    u32 color
);

void gfx_buffer_clear_dirty(naq_gfx_buffer_t* buffer);

int gfx_buffer_present(
    naq_gfx_buffer_t* buffer,
    i32 dst_x,
    i32 dst_y
);

int gfx_buffer_present_dirty(
    naq_gfx_buffer_t* buffer,
    i32 dst_x,
    i32 dst_y
);

#endif
