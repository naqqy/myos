#include "idt64.h"
#include "gdt64.h"
#include "../../k64_console.h"
#include "syscall64.h"
#include "../../debug/bootlog64.h"
#include "../../proc/process64.h"

#define IDT64_ENTRIES 256

#define IDT_TYPE_INTERRUPT_GATE 0x8E
#define IDT_TYPE_USER_TRAP_GATE 0xEF

typedef struct idt64_entry {
    nq_u16 offset_low;
    nq_u16 selector;
    nq_u8  ist;
    nq_u8  type_attr;
    nq_u16 offset_mid;
    nq_u32 offset_high;
    nq_u32 zero;
} __attribute__((packed)) idt64_entry_t;

typedef struct idt64_ptr {
    nq_u16 limit;
    nq_u64 base;
} __attribute__((packed)) idt64_ptr_t;

extern void idt_load64(idt64_ptr_t* idtr);
extern nq_u64 isr64_stub_table[];

static idt64_entry_t idt64[IDT64_ENTRIES];
static idt64_ptr_t idt64_ptr;

extern void irq64_stub_0(void);
extern void irq64_stub_1(void);

extern void syscall64_stub(void);

static const char* exception_names64[32] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 Floating Point",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating Point",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

static void mem_zero(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static nq_u16 read_cs(void) {
    nq_u16 cs;

    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    return cs;
}

static nq_u64 read_cr2(void) {
    nq_u64 cr2;

    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    return cr2;
}

static void idt64_set_gate(nq_u8 vector, nq_u64 handler, nq_u16 selector, nq_u8 ist, nq_u8 type_attr) {
    idt64[vector].offset_low = (nq_u16)(handler & 0xFFFFu);
    idt64[vector].selector = selector;
    idt64[vector].ist = ist & 0x7u;
    idt64[vector].type_attr = type_attr;
    idt64[vector].offset_mid = (nq_u16)((handler >> 16) & 0xFFFFu);
    idt64[vector].offset_high = (nq_u32)((handler >> 32) & 0xFFFFFFFFu);
    idt64[vector].zero = 0;
}

void idt64_install_irq_gates(void) {
    idt64_set_gate(
        32,
        (nq_u64)irq64_stub_0,
        GDT64_KERNEL_CODE,
        0,
        IDT_TYPE_INTERRUPT_GATE
    );

    idt64_set_gate(
        33,
        (nq_u64)irq64_stub_1,
        GDT64_KERNEL_CODE,
        0,
        IDT_TYPE_INTERRUPT_GATE
    );

    bootlog64_print("IDT64 IRQ gates installed.\n");
}

void idt64_init(void) {
    mem_zero(idt64, sizeof(idt64));

    for (nq_u8 i = 0; i < 32; i++) {
        nq_u8 ist = 0;

        if (i == 8) {
            ist = GDT64_IST_DOUBLE_FAULT;
        } else if (i == 14) {
            ist = GDT64_IST_PAGE_FAULT;
        }

        idt64_set_gate(
            i,
            isr64_stub_table[i],
            GDT64_KERNEL_CODE,
            ist,
            IDT_TYPE_INTERRUPT_GATE
        );
    }

    idt64_ptr.limit = (nq_u16)(sizeof(idt64) - 1);
    idt64_ptr.base = (nq_u64)&idt64;

    idt_load64(&idt64_ptr);

    bootlog64_print("IDT64 loaded.\n");
}

void idt64_install_syscall_gate(void) {
    idt64_set_gate(
        SYSCALL64_VECTOR,
        (nq_u64)syscall64_stub,
        GDT64_KERNEL_CODE,
        0,
        IDT_TYPE_USER_TRAP_GATE
    );

    bootlog64_print("IDT64 syscall gate installed.\n");
}

void exception_handler64(interrupt_frame64_t* frame) {
    __asm__ volatile ("cli");

    /*
     * A fault raised by ring 3 code is the app's problem, not the kernel's.
     * Report it, kill that process and keep the system running. The task
     * never resumes, so abandoning this exception stack is fine.
     */
    if (frame && (frame->cs & 3u) == 3u && process64_current_pid() != 0) {
        k64_print("\n[user] pid ");
        k64_print_dec64(process64_current_pid());
        k64_print(" killed: ");

        if (frame->vector < 32) {
            k64_print(exception_names64[frame->vector]);
        } else {
            k64_print("Unknown exception");
        }

        k64_print(" rip=");
        k64_print_hex64(frame->rip);
        k64_print(" err=");
        k64_print_hex64(frame->error_code);

        if (frame->vector == 14) {
            k64_print(" cr2=");
            k64_print_hex64(read_cr2());
        }

        k64_print("\n");

        process64_exit();
    }

    k64_print("\n\nCPU EXCEPTION 64\n");

    if (!frame) {
        k64_print("No frame.\n");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    k64_print("Vector: ");
    k64_print_dec64(frame->vector);
    k64_print(" ");

    if (frame->vector < 32) {
        k64_print(exception_names64[frame->vector]);
    } else {
        k64_print("Unknown");
    }

    k64_print("\nError Code: ");
    k64_print_hex64(frame->error_code);

    k64_print("\nRIP: ");
    k64_print_hex64(frame->rip);

    k64_print("\nCS: ");
    k64_print_hex64(frame->cs);

    k64_print("\nRFLAGS: ");
    k64_print_hex64(frame->rflags);

    k64_print("\nRAX: ");
    k64_print_hex64(frame->rax);

    k64_print("\nRBX: ");
    k64_print_hex64(frame->rbx);

    k64_print("\nRCX: ");
    k64_print_hex64(frame->rcx);

    k64_print("\nRDX: ");
    k64_print_hex64(frame->rdx);

    if (frame->vector == 14) {
        k64_print("\nCR2: ");
        k64_print_hex64(read_cr2());
    }

    k64_print("\n\nHALTED AFTER EXCEPTION\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}