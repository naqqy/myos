#ifndef PMM_H
#define PMM_H

typedef unsigned int u32;

#define PMM_FRAME_SIZE 4096

void pmm_init(void);
u32 pmm_alloc_frame(void);
void pmm_free_frame(u32 phys_addr);

void pmm_reserve_region(u32 start, u32 length);
void pmm_unreserve_region(u32 start, u32 length);

u32 pmm_total_frames(void);
u32 pmm_used_frames(void);
u32 pmm_free_frames(void);

u32 pmm_top_address(void);

#endif