#ifndef PMM64_H
#define PMM64_H

#include "naq_boot_info.h"

#define PMM64_FRAME_SIZE 4096ull

/*
 * Early PMM64 cap:
 * 4 GiB / 4096 = 1,048,576 frames
 * bitmap size = 1,048,576 / 8 = 131,072 bytes
 */
#define PMM64_MAX_PHYS_ADDR  0x0000000100000000ull
#define PMM64_MAX_FRAMES     (PMM64_MAX_PHYS_ADDR / PMM64_FRAME_SIZE)
#define PMM64_BITMAP_SIZE    (PMM64_MAX_FRAMES / 8ull)

int    pmm64_init(const naq_boot_info_t* boot);

nq_u64 pmm64_alloc_frame(void);
void   pmm64_free_frame(nq_u64 phys_addr);

nq_u64 pmm64_alloc_contiguous_frames(nq_u64 frame_count);
void   pmm64_free_contiguous_frames(nq_u64 phys_addr, nq_u64 frame_count);

void   pmm64_reserve_region(nq_u64 base, nq_u64 length);
void   pmm64_unreserve_region(nq_u64 base, nq_u64 length);

nq_u64 pmm64_managed_frames(void);
nq_u64 pmm64_usable_frames(void);
nq_u64 pmm64_free_frames(void);
nq_u64 pmm64_used_frames(void);
nq_u64 pmm64_top_address(void);

void   pmm64_print_summary(void);

#endif