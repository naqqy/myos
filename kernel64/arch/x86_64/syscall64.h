#ifndef SYSCALL64_H
#define SYSCALL64_H

#include "naq_boot_info.h"

#define SYSCALL64_VECTOR 0x80

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
#define SYS64_GFX_INFO       12
#define SYS64_GFX_CLEAR      13
#define SYS64_GFX_PRESENT    14
#define SYS64_GFX_DRAW_RECT  15
#define SYS64_GFX_BLIT       16
#define SYS64_GFX_SET_MODE   17
#define SYS64_GFX_GET_MODE   18
#define SYS64_WIN_CREATE      19
#define SYS64_WIN_DESTROY     20
#define SYS64_WIN_PRESENT     21
#define SYS64_WIN_SET_TITLE   22
#define SYS64_WIN_GET_EVENT   23
#define SYS64_WIN_CREATE_EX   24

#define NAQ_WIN_EVENT_NONE   0
#define NAQ_WIN_EVENT_KEY    1
#define NAQ_WIN_EVENT_FOCUS  2
#define NAQ_WIN_EVENT_CLOSE  3
#define NAQ_WIN_EVENT_RESIZE 4
#define NAQ_WIN_EVENT_MOUSE  5

#define NAQ_WIN_FLAG_BORDERED    0x00000000u
#define NAQ_WIN_FLAG_BORDERLESS  0x00000001u
#define NAQ_WIN_FLAG_FULLSCREEN  0x00000002u
#define NAQ_WIN_FLAG_GAME        0x00000004u

typedef struct naq_stat {
    nq_u64 size;
    nq_u32 type;
    nq_u32 reserved;
} naq_stat_t;

#define NAQ_STAT_TYPE_UNKNOWN 0
#define NAQ_STAT_TYPE_FILE    1
#define NAQ_STAT_TYPE_DIR     2

#define SYS64_OK          0
#define SYS64_EINVAL     22
#define SYS64_ENOSYS     38
#define SYS64_EFAULT     14

#define SYS64_ERR(x) ((nq_u64)(0ull - (nq_u64)(x)))

typedef struct syscall64_frame {
    nq_u64 r15;
    nq_u64 r14;
    nq_u64 r13;
    nq_u64 r12;
    nq_u64 r11;
    nq_u64 r10;
    nq_u64 r9;
    nq_u64 r8;
    nq_u64 rdi;
    nq_u64 rsi;
    nq_u64 rbp;
    nq_u64 rbx;
    nq_u64 rdx;
    nq_u64 rcx;
    nq_u64 rax;

    /*
     * Pushed by syscall64_stub before jumping common.
     */
    nq_u64 vector;

    /*
     * Pushed automatically by CPU on privilege transition.
     */
    nq_u64 rip;
    nq_u64 cs;
    nq_u64 rflags;
    nq_u64 rsp;
    nq_u64 ss;
} syscall64_frame_t;

void syscall64_init(void);
void syscall64_handler(syscall64_frame_t* frame);

#endif