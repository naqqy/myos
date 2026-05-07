#ifndef NAQ_BOOT_INFO_H
#define NAQ_BOOT_INFO_H

typedef unsigned char       nq_u8;
typedef unsigned short      nq_u16;
typedef unsigned int        nq_u32;
typedef unsigned long long  nq_u64;

#define NAQ_BOOT_INFO_MAGIC   0x314F464E4951424EULL  /* "NBINFO1" marker */
#define NAQ_BOOT_INFO_VERSION 1u

/*
 * Assembly-visible offsets.
 * Keep these in sync with naq_boot_info_t.
 */
#define NAQ_BOOT_INFO_MAGIC_OFFSET        0u
#define NAQ_BOOT_INFO_VERSION_OFFSET      8u

#define NAQ_BOOT_INFO_FRAMEBUFFER_BASE    16u
#define NAQ_BOOT_INFO_FRAMEBUFFER_SIZE    24u
#define NAQ_BOOT_INFO_FRAMEBUFFER_WIDTH   32u
#define NAQ_BOOT_INFO_FRAMEBUFFER_HEIGHT  36u
#define NAQ_BOOT_INFO_FRAMEBUFFER_PPSL    40u
#define NAQ_BOOT_INFO_FRAMEBUFFER_FORMAT  44u

#define NAQ_BOOT_INFO_KERNEL_LOAD_ADDR    48u
#define NAQ_BOOT_INFO_KERNEL_SIZE         56u

#define NAQ_BOOT_INFO_STACK_BASE          64u
#define NAQ_BOOT_INFO_STACK_SIZE          72u
#define NAQ_BOOT_INFO_STACK_TOP           80u

#define NAQ_BOOT_INFO_MEMORY_MAP          88u
#define NAQ_BOOT_INFO_MEMORY_MAP_SIZE     96u
#define NAQ_BOOT_INFO_MEMORY_DESC_SIZE    104u
#define NAQ_BOOT_INFO_MEMORY_DESC_VERSION 112u

typedef struct naq_boot_info {
    nq_u64 magic;
    nq_u64 version;

    nq_u64 framebuffer_base;
    nq_u64 framebuffer_size;

    nq_u32 framebuffer_width;
    nq_u32 framebuffer_height;
    nq_u32 framebuffer_pixels_per_scanline;
    nq_u32 framebuffer_pixel_format;

    nq_u64 kernel_load_addr;
    nq_u64 kernel_size;

    nq_u64 stack_base;
    nq_u64 stack_size;
    nq_u64 stack_top;

    nq_u64 memory_map;
    nq_u64 memory_map_size;
    nq_u64 memory_map_desc_size;
    nq_u32 memory_map_desc_version;
    nq_u32 reserved0;

    nq_u64 reserved1;
    nq_u64 reserved2;
    nq_u64 reserved3;
    nq_u64 reserved4;
} naq_boot_info_t;

#endif