#include "gdt64.h"
#include "../../k64_console.h"

#define GDT64_ACCESS_KERNEL_CODE 0x9A
#define GDT64_ACCESS_KERNEL_DATA 0x92
#define GDT64_ACCESS_USER_DATA   0xF2
#define GDT64_ACCESS_USER_CODE   0xFA
#define GDT64_ACCESS_TSS         0x89

#define GDT64_FLAG_LONG_MODE     0x20
#define GDT64_FLAG_GRANULARITY   0x80

#define GDT64_IST_STACK_SIZE     16384u

typedef struct gdt64_tss {
    nq_u32 reserved0;

    nq_u64 rsp0;
    nq_u64 rsp1;
    nq_u64 rsp2;

    nq_u64 reserved1;

    nq_u64 ist1;
    nq_u64 ist2;
    nq_u64 ist3;
    nq_u64 ist4;
    nq_u64 ist5;
    nq_u64 ist6;
    nq_u64 ist7;

    nq_u64 reserved2;
    nq_u16 reserved3;
    nq_u16 iomap_base;
} __attribute__((packed)) gdt64_tss_t;

typedef struct gdt64_tss_desc {
    nq_u16 limit_low;
    nq_u16 base_low;
    nq_u8  base_mid1;
    nq_u8  access;
    nq_u8  flags_limit_high;
    nq_u8  base_mid2;
    nq_u32 base_high;
    nq_u32 reserved;
} __attribute__((packed)) gdt64_tss_desc_t;

typedef struct gdt64_table {
    nq_u64 null_desc;
    nq_u64 kernel_code;
    nq_u64 kernel_data;
    nq_u64 user_data;
    nq_u64 user_code;
    gdt64_tss_desc_t tss_desc;
} __attribute__((packed)) gdt64_table_t;

typedef struct gdt64_ptr {
    nq_u16 limit;
    nq_u64 base;
} __attribute__((packed)) gdt64_ptr_t;

extern void gdt64_load(gdt64_ptr_t* ptr);
extern void tss64_load(nq_u16 selector);

static gdt64_table_t gdt64;
static gdt64_ptr_t gdt64_ptr;
static gdt64_tss_t tss64;

static nq_u8 df_stack[GDT64_IST_STACK_SIZE] __attribute__((aligned(16)));
static nq_u8 pf_stack[GDT64_IST_STACK_SIZE] __attribute__((aligned(16)));

static void mem_zero(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static nq_u64 make_descriptor(nq_u32 base, nq_u32 limit, nq_u8 access, nq_u8 flags) {
    nq_u64 desc = 0;

    desc |= (nq_u64)(limit & 0xFFFFu);
    desc |= (nq_u64)(base & 0xFFFFFFu) << 16;
    desc |= (nq_u64)access << 40;
    desc |= (nq_u64)((limit >> 16) & 0x0Fu) << 48;
    desc |= (nq_u64)(flags & 0xF0u) << 48;
    desc |= (nq_u64)((base >> 24) & 0xFFu) << 56;

    return desc;
}

static void set_tss_descriptor(gdt64_tss_desc_t* desc, nq_u64 base, nq_u32 limit) {
    desc->limit_low = (nq_u16)(limit & 0xFFFFu);
    desc->base_low = (nq_u16)(base & 0xFFFFu);
    desc->base_mid1 = (nq_u8)((base >> 16) & 0xFFu);
    desc->access = GDT64_ACCESS_TSS;
    desc->flags_limit_high = (nq_u8)((limit >> 16) & 0x0Fu);
    desc->base_mid2 = (nq_u8)((base >> 24) & 0xFFu);
    desc->base_high = (nq_u32)((base >> 32) & 0xFFFFFFFFu);
    desc->reserved = 0;
}

void gdt64_init(const naq_boot_info_t* boot) {
    mem_zero(&gdt64, sizeof(gdt64));
    mem_zero(&gdt64_ptr, sizeof(gdt64_ptr));
    mem_zero(&tss64, sizeof(tss64));

    gdt64.null_desc = 0;

    gdt64.kernel_code =
        make_descriptor(0, 0, GDT64_ACCESS_KERNEL_CODE, GDT64_FLAG_LONG_MODE);

    gdt64.kernel_data =
        make_descriptor(0, 0, GDT64_ACCESS_KERNEL_DATA, 0);

    gdt64.user_data =
        make_descriptor(0, 0, GDT64_ACCESS_USER_DATA, 0);

    gdt64.user_code =
        make_descriptor(0, 0, GDT64_ACCESS_USER_CODE, GDT64_FLAG_LONG_MODE);

    if (boot && boot->magic == NAQ_BOOT_INFO_MAGIC) {
        tss64.rsp0 = boot->stack_top;
    }

    tss64.ist1 = (nq_u64)(df_stack + sizeof(df_stack));
    tss64.ist2 = (nq_u64)(pf_stack + sizeof(pf_stack));

    /*
     * No I/O bitmap for now.
     */
    tss64.iomap_base = sizeof(tss64);

    set_tss_descriptor(
        &gdt64.tss_desc,
        (nq_u64)&tss64,
        (nq_u32)(sizeof(tss64) - 1)
    );

    gdt64_ptr.limit = (nq_u16)(sizeof(gdt64) - 1);
    gdt64_ptr.base = (nq_u64)&gdt64;

    gdt64_load(&gdt64_ptr);
    tss64_load(GDT64_TSS);

    k64_print("GDT64/TSS64 loaded.\n");
}