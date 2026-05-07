#include "pmm.h"
#include "memmap.h"

typedef unsigned char u8;
typedef unsigned int  u32;

#define PMM_BITMAP_MAX_FRAMES 32768
#define PMM_BITMAP_SIZE       (PMM_BITMAP_MAX_FRAMES / 8)

static u8 pmm_bitmap[PMM_BITMAP_SIZE];
static u32 pmm_max_frame = 0;
static u32 pmm_total_usable_frames = 0;
static u32 pmm_used_frame_count = 0;

static u32 align_up(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}

static u32 align_down(u32 value, u32 align) {
    return value & ~(align - 1);
}

static void bitmap_set(u32 frame) {
    pmm_bitmap[frame / 8] |= (u8)(1u << (frame % 8));
}

static void bitmap_clear(u32 frame) {
    pmm_bitmap[frame / 8] &= (u8)~(1u << (frame % 8));
}

static int bitmap_test(u32 frame) {
    return (pmm_bitmap[frame / 8] & (1u << (frame % 8))) != 0;
}

static void reserve_frame(u32 frame) {
    if (frame >= pmm_max_frame) return;
    if (!bitmap_test(frame)) {
        bitmap_set(frame);
        pmm_used_frame_count++;
    }
}

static void unreserve_frame(u32 frame) {
    if (frame >= pmm_max_frame) return;
    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        if (pmm_used_frame_count > 0) {
            pmm_used_frame_count--;
        }
    }
}

void pmm_reserve_region(u32 start, u32 length) {
    u32 begin = align_down(start, PMM_FRAME_SIZE);
    u32 end   = align_up(start + length, PMM_FRAME_SIZE);

    for (u32 addr = begin; addr < end; addr += PMM_FRAME_SIZE) {
        reserve_frame(addr / PMM_FRAME_SIZE);
    }
}

void pmm_unreserve_region(u32 start, u32 length) {
    u32 begin = align_up(start, PMM_FRAME_SIZE);
    u32 end   = align_down(start + length, PMM_FRAME_SIZE);

    for (u32 addr = begin; addr < end; addr += PMM_FRAME_SIZE) {
        unreserve_frame(addr / PMM_FRAME_SIZE);
    }
}

void pmm_init(void) {
    for (u32 i = 0; i < PMM_BITMAP_SIZE; i++) {
        pmm_bitmap[i] = 0xFF;
    }

    pmm_max_frame = 0;
    pmm_total_usable_frames = 0;
    pmm_used_frame_count = 0;

    for (u32 i = 0; i < memmap_count(); i++) {
        const memmap_entry_t* e = memmap_entry(i);
        if (!e) continue;
        if (e->base_high != 0 || e->length_high != 0) continue;

        u32 region_end = e->base_low + e->length_low;
        u32 end_frame = align_up(region_end, PMM_FRAME_SIZE) / PMM_FRAME_SIZE;

        if (end_frame > pmm_max_frame) {
            pmm_max_frame = end_frame;
        }
    }

    if (pmm_max_frame > PMM_BITMAP_MAX_FRAMES) {
        pmm_max_frame = PMM_BITMAP_MAX_FRAMES;
    }

    pmm_used_frame_count = pmm_max_frame;

    for (u32 i = 0; i < memmap_count(); i++) {
        const memmap_entry_t* e = memmap_entry(i);
        if (!e) continue;
        if (e->type != MEMMAP_TYPE_USABLE) continue;
        if (e->base_high != 0 || e->length_high != 0) continue;

        u32 start = e->base_low;
        u32 len   = e->length_low;
        if (len == 0) continue;

        u32 begin = align_up(start, PMM_FRAME_SIZE);
        u32 end   = align_down(start + len, PMM_FRAME_SIZE);

        if (end > begin) {
            pmm_total_usable_frames += (end - begin) / PMM_FRAME_SIZE;
            pmm_unreserve_region(begin, end - begin);
        }
    }
}

u32 pmm_alloc_frame(void) {
    for (u32 frame = 0; frame < pmm_max_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            pmm_used_frame_count++;
            return frame * PMM_FRAME_SIZE;
        }
    }

    return 0;
}

void pmm_free_frame(u32 phys_addr) {
    if ((phys_addr & (PMM_FRAME_SIZE - 1)) != 0) {
        return;
    }

    u32 frame = phys_addr / PMM_FRAME_SIZE;
    unreserve_frame(frame);
}

u32 pmm_total_frames(void) {
    return pmm_total_usable_frames;
}

u32 pmm_used_frames(void) {
    return pmm_used_frame_count;
}

u32 pmm_free_frames(void) {
    if (pmm_total_usable_frames > pmm_used_frame_count) {
        return pmm_total_usable_frames - pmm_used_frame_count;
    }
    return 0;
}

u32 pmm_top_address(void) {
    return pmm_max_frame * PMM_FRAME_SIZE;
}