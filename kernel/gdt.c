#include "gdt.h"

typedef struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_middle;
    u8  access;
    u8  granularity;
    u8  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct tss_entry {
    u32 prev_tss;
    u32 esp0;
    u32 ss0;
    u32 esp1;
    u32 ss1;
    u32 esp2;
    u32 ss2;
    u32 cr3;
    u32 eip;
    u32 eflags;
    u32 eax;
    u32 ecx;
    u32 edx;
    u32 ebx;
    u32 esp;
    u32 ebp;
    u32 esi;
    u32 edi;
    u32 es;
    u32 cs;
    u32 ss;
    u32 ds;
    u32 fs;
    u32 gs;
    u32 ldt;
    u16 trap;
    u16 iomap_base;
} __attribute__((packed)) tss_entry_t;

static gdt_entry_t gdt[6];
static gdt_ptr_t gdt_ptr;
static tss_entry_t tss;

extern void gdt_flush(u32 gdt_ptr_addr);
extern void tss_flush(void);

static void mem_zero(void* ptr, u32 size) {
    u8* p = (u8*)ptr;

    for (u32 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void gdt_set_gate(u32 index, u32 base, u32 limit, u8 access, u8 granularity) {
    gdt[index].base_low = (u16)(base & 0xFFFF);
    gdt[index].base_middle = (u8)((base >> 16) & 0xFF);
    gdt[index].base_high = (u8)((base >> 24) & 0xFF);

    gdt[index].limit_low = (u16)(limit & 0xFFFF);
    gdt[index].granularity = (u8)((limit >> 16) & 0x0F);
    gdt[index].granularity |= granularity & 0xF0;

    gdt[index].access = access;
}

static void tss_write(u32 index, u16 ss0, u32 esp0) {
    u32 base = (u32)&tss;
    u32 limit = sizeof(tss_entry_t) - 1;

    /*
     * access 0x89 = present, ring 0, available 32-bit TSS
     * granularity 0x40 = 32-bit
     */
    gdt_set_gate(index, base, limit, 0x89, 0x40);

    mem_zero(&tss, sizeof(tss_entry_t));

    tss.ss0 = ss0;
    tss.esp0 = esp0;

    /*
     * These selectors are what ring 3 code will use when/if the CPU
     * restores segments through the TSS path. RPL 3 is included.
     */
    tss.cs = GDT_USER_CODE;
    tss.ss = GDT_USER_DATA;
    tss.ds = GDT_USER_DATA;
    tss.es = GDT_USER_DATA;
    tss.fs = GDT_USER_DATA;
    tss.gs = GDT_USER_DATA;

    /*
     * Disable the I/O permission bitmap.
     */
    tss.iomap_base = sizeof(tss_entry_t);
}

void tss_set_kernel_stack(u32 esp0) {
    tss.esp0 = esp0;
}

void gdt_init(void) {
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (u32)&gdt;

    /*
     * GDT layout:
     * 0: null
     * 1: kernel code
     * 2: kernel data
     * 3: user code
     * 4: user data
     * 5: TSS
     */
    gdt_set_gate(0, 0, 0, 0, 0);

    /*
     * access:
     * 0x9A = kernel code, present, ring 0, executable/readable
     * 0x92 = kernel data, present, ring 0, writable
     *
     * granularity:
     * 0xCF = 4 KB granularity, 32-bit, limit high bits set
     */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /*
     * 0xFA = user code, present, ring 3, executable/readable
     * 0xF2 = user data, present, ring 3, writable
     */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    tss_write(5, GDT_KERNEL_DATA, 0);

    gdt_flush((u32)&gdt_ptr);
    tss_flush();
}