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

#define SYS64_WRITE      0
#define SYS64_EXIT       1
#define SYS64_GET_TICKS  2
#define SYS64_YIELD      3
#define SYS64_GETPID     4
#define SYS64_SLEEP      5
#define SYS64_INPUT_POLL_KEY  6
#define SYS64_INPUT_GET_KEY   7
#define SYS64_OPEN            8
#define SYS64_READ            9
#define SYS64_CLOSE           10
#define SYS64_STAT            11

#define NAQ_STAT_TYPE_UNKNOWN 0
#define NAQ_STAT_TYPE_FILE    1
#define NAQ_STAT_TYPE_DIR     2

typedef struct naq_stat {
    u64 size;
    u32 type;
    u32 reserved;
} naq_stat_t;

#define NAQ_STAT_TYPE_UNKNOWN 0
#define NAQ_STAT_TYPE_FILE    1
#define NAQ_STAT_TYPE_DIR     2

i64 syscall0(u64 num);
i64 syscall1(u64 num, u64 a0);
i64 syscall2(u64 num, u64 a0, u64 a1);
i64 syscall3(u64 num, u64 a0, u64 a1, u64 a2);

void exit(int code);
i64 write(int fd, const void* buf, size_t len);
u64 get_ticks(void);
void yield(void);
u64 getpid(void);
i64 sleep_ticks(u64 ticks);

size_t strlen(const char* s);
void* memset(void* dst, int value, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

void putchar(int c);
void puts(const char* s);

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

int input_poll_key(void);
int input_get_key(void);

int open(const char* path, u64 flags);
i64 read(int fd, void* buf, size_t len);
int close(int fd);
int stat(const char* path, naq_stat_t* out);

#endif