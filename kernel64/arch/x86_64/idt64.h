#ifndef IDT64_H
#define IDT64_H

#include "naq_boot_info.h"

typedef struct interrupt_frame64 {
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

    nq_u64 vector;
    nq_u64 error_code;

    nq_u64 rip;
    nq_u64 cs;
    nq_u64 rflags;
} interrupt_frame64_t;

typedef struct irq_frame64 {
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

    nq_u64 irq;

    nq_u64 rip;
    nq_u64 cs;
    nq_u64 rflags;
} irq_frame64_t;

void idt64_init(void);
void idt64_install_irq_gates(void);

void exception_handler64(interrupt_frame64_t* frame);

#endif