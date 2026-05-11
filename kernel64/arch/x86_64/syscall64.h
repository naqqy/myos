#ifndef SYSCALL64_H
#define SYSCALL64_H

#include "naq_boot_info.h"

#define SYSCALL64_VECTOR 0x80

#define SYS64_WRITE      0
#define SYS64_EXIT       1
#define SYS64_GET_TICKS  2

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