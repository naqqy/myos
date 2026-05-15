#include "pmm64.h"
#include "memmap64.h"
#include "../k64_console.h"

static nq_u8 pmm64_bitmap[PMM64_BITMAP_SIZE];

static nq_u64 pmm64_max_frame = 0;
static nq_u64 pmm64_usable_frame_count = 0;
static nq_u64 pmm64_next_search = 0;
static int pmm64_ready = 0;

#define PMM64_EARLY_KERNEL_RESERVE_SIZE (96ull * 1024ull * 1024ull)
/* Fixed POC user physical reservation removed by per-process VM. */

static nq_u64 align_down64(nq_u64 value, nq_u64 align) {
    return value & ~(align - 1ull);
}

static nq_u64 align_up64(nq_u64 value, nq_u64 align) {
    nq_u64 mask = align - 1ull;

    if ((value & mask) == 0) {
        return value;
    }

    if (value > 0xFFFFFFFFFFFFFFFFull - mask) {
        return 0xFFFFFFFFFFFFFFFFull & ~mask;
    }

    return (value + mask) & ~mask;
}

static int checked_end64(nq_u64 base, nq_u64 length, nq_u64* out_end) {
    if (length == 0) {
        return 0;
    }

    if (base > 0xFFFFFFFFFFFFFFFFull - length) {
        return 0;
    }

    if (out_end) {
        *out_end = base + length;
    }

    return 1;
}

static void mem_set(void* ptr, nq_u8 value, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = value;
    }
}

static int frame_in_range(nq_u64 frame) {
    return frame < pmm64_max_frame && frame < PMM64_MAX_FRAMES;
}

static void bitmap_set(nq_u64 frame) {
    if (frame >= PMM64_MAX_FRAMES) {
        return;
    }

    pmm64_bitmap[frame / 8ull] |= (nq_u8)(1u << (frame % 8ull));
}

static void bitmap_clear(nq_u64 frame) {
    if (frame >= PMM64_MAX_FRAMES) {
        return;
    }

    pmm64_bitmap[frame / 8ull] &= (nq_u8)~(1u << (frame % 8ull));
}

static int bitmap_test(nq_u64 frame) {
    if (frame >= PMM64_MAX_FRAMES) {
        return 1;
    }

    return (pmm64_bitmap[frame / 8ull] & (1u << (frame % 8ull))) != 0;
}

static void mark_frame_used(nq_u64 frame) {
    if (!frame_in_range(frame)) {
        return;
    }

    bitmap_set(frame);
}

static void mark_frame_free(nq_u64 frame) {
    if (!frame_in_range(frame)) {
        return;
    }

    bitmap_clear(frame);
}

static void free_usable_region(nq_u64 base, nq_u64 length) {
    nq_u64 end;

    if (!checked_end64(base, length, &end)) {
        return;
    }

    if (base >= PMM64_MAX_PHYS_ADDR) {
        return;
    }

    if (end > PMM64_MAX_PHYS_ADDR) {
        end = PMM64_MAX_PHYS_ADDR;
    }

    nq_u64 start = align_up64(base, PMM64_FRAME_SIZE);
    nq_u64 stop  = align_down64(end, PMM64_FRAME_SIZE);

    if (stop <= start) {
        return;
    }

    for (nq_u64 addr = start; addr < stop; addr += PMM64_FRAME_SIZE) {
        nq_u64 frame = addr / PMM64_FRAME_SIZE;

        if (frame_in_range(frame)) {
            if (bitmap_test(frame)) {
                mark_frame_free(frame);
                pmm64_usable_frame_count++;
            }
        }
    }
}

void pmm64_reserve_region(nq_u64 base, nq_u64 length) {
    nq_u64 end;

    if (!checked_end64(base, length, &end)) {
        return;
    }

    if (base >= PMM64_MAX_PHYS_ADDR) {
        return;
    }

    if (end > PMM64_MAX_PHYS_ADDR) {
        end = PMM64_MAX_PHYS_ADDR;
    }

    nq_u64 start = align_down64(base, PMM64_FRAME_SIZE);
    nq_u64 stop  = align_up64(end, PMM64_FRAME_SIZE);

    for (nq_u64 addr = start; addr < stop; addr += PMM64_FRAME_SIZE) {
        nq_u64 frame = addr / PMM64_FRAME_SIZE;
        mark_frame_used(frame);
    }

    if (pmm64_next_search > start / PMM64_FRAME_SIZE) {
        pmm64_next_search = start / PMM64_FRAME_SIZE;
    }
}

void pmm64_unreserve_region(nq_u64 base, nq_u64 length) {
    nq_u64 end;

    if (!checked_end64(base, length, &end)) {
        return;
    }

    if (base >= PMM64_MAX_PHYS_ADDR) {
        return;
    }

    if (end > PMM64_MAX_PHYS_ADDR) {
        end = PMM64_MAX_PHYS_ADDR;
    }

    nq_u64 start = align_up64(base, PMM64_FRAME_SIZE);
    nq_u64 stop  = align_down64(end, PMM64_FRAME_SIZE);

    for (nq_u64 addr = start; addr < stop; addr += PMM64_FRAME_SIZE) {
        nq_u64 frame = addr / PMM64_FRAME_SIZE;
        mark_frame_free(frame);
    }
}

static void pmm64_reserve_boot_regions(const naq_boot_info_t* boot) {
    /*
     * Keep low memory protected.
     */
    pmm64_reserve_region(0, 0x00100000ull);

    /*
    * Kernel image loaded by the UEFI loader.
    *
    * Important:
    * The flat KERNEL64.BIN file may not include all zero-filled/static storage
    * in a way the loader's file size reflects. Until we pass exact kernel end
    * metadata, reserve at least 4 MiB from the kernel load address.
    */
    nq_u64 kernel_reserved_size = boot->kernel_size;

    if (kernel_reserved_size < PMM64_EARLY_KERNEL_RESERVE_SIZE) {
        kernel_reserved_size = PMM64_EARLY_KERNEL_RESERVE_SIZE;
    }

    pmm64_reserve_region(
        boot->kernel_load_addr,
        kernel_reserved_size
    );

    /*
     * Kernel stack allocated by the UEFI loader.
     */
    pmm64_reserve_region(
        boot->stack_base,
        boot->stack_size
    );

    /*
     * UEFI memory map buffer.
     */
    pmm64_reserve_region(
        boot->memory_map,
        boot->memory_map_size
    );

    /*
     * Boot info struct itself.
     */
    pmm64_reserve_region(
        (nq_u64)boot,
        sizeof(*boot)
    );

    /*
     * Framebuffer memory must never be handed out as normal RAM.
     */
    pmm64_reserve_region(
        boot->framebuffer_base,
        boot->framebuffer_size
    );

    /*
     * Static PMM bitmap storage.
     */
    pmm64_reserve_region(
        (nq_u64)pmm64_bitmap,
        sizeof(pmm64_bitmap)
    );

    if (boot->initrd_base && boot->initrd_size) {
        pmm64_reserve_region(
            boot->initrd_base,
            boot->initrd_size
        );
    }

    /*
     * Per-process user address spaces now allocate normal PMM frames for
     * user code/data/stack, so the old fixed POC user physical reservation
     * is no longer needed.
     */
}

int pmm64_init(const naq_boot_info_t* boot) {
    if (!boot ||
        boot->magic != NAQ_BOOT_INFO_MAGIC ||
        boot->version != NAQ_BOOT_INFO_VERSION) {
        return 0;
    }

    /*
     * Start with everything marked used/reserved.
     */
    mem_set(pmm64_bitmap, 0xFF, sizeof(pmm64_bitmap));

    pmm64_usable_frame_count = 0;
    pmm64_next_search = 0;
    pmm64_ready = 0;

    nq_u64 highest = memmap64_highest_usable_address();

    if (highest > PMM64_MAX_PHYS_ADDR) {
        highest = PMM64_MAX_PHYS_ADDR;
    }

    pmm64_max_frame = align_up64(highest, PMM64_FRAME_SIZE) / PMM64_FRAME_SIZE;

    if (pmm64_max_frame > PMM64_MAX_FRAMES) {
        pmm64_max_frame = PMM64_MAX_FRAMES;
    }

    if (pmm64_max_frame == 0) {
        return 0;
    }

    /*
     * Free every usable frame from the parsed UEFI map.
     */
    nq_u64 count = memmap64_region_count();

    for (nq_u64 i = 0; i < count; i++) {
        const k64_mem_region_t* r = memmap64_region(i);

        if (!r) {
            continue;
        }

        if (r->type != K64_MEM_REGION_USABLE) {
            continue;
        }

        free_usable_region(r->base, r->length);
    }

    /*
     * Re-reserve everything the kernel/loader needs to keep.
     */
    pmm64_reserve_boot_regions(boot);

    pmm64_ready = 1;

    return 1;
}

nq_u64 pmm64_alloc_frame(void) {
    if (!pmm64_ready || pmm64_max_frame == 0) {
        return 0;
    }

    for (nq_u64 pass = 0; pass < 2; pass++) {
        nq_u64 start = (pass == 0) ? pmm64_next_search : 0;
        nq_u64 end   = (pass == 0) ? pmm64_max_frame : pmm64_next_search;

        for (nq_u64 frame = start; frame < end; frame++) {
            if (!bitmap_test(frame)) {
                mark_frame_used(frame);
                pmm64_next_search = frame + 1;

                return frame * PMM64_FRAME_SIZE;
            }
        }
    }

    k64_print("PMM64: out of free frames. free_frames=");
    k64_print_dec64(pmm64_free_frames());
    k64_print("\n");

    return 0;
}

void pmm64_free_frame(nq_u64 phys_addr) {
    if (!pmm64_ready) {
        return;
    }

    if ((phys_addr & (PMM64_FRAME_SIZE - 1ull)) != 0) {
        return;
    }

    if (phys_addr >= PMM64_MAX_PHYS_ADDR) {
        return;
    }

    nq_u64 frame = phys_addr / PMM64_FRAME_SIZE;

    if (!frame_in_range(frame)) {
        return;
    }

    mark_frame_free(frame);

    if (frame < pmm64_next_search) {
        pmm64_next_search = frame;
    }
}

static int pmm64_frame_range_is_free(nq_u64 start_frame, nq_u64 frame_count) {
    if (frame_count == 0) {
        return 0;
    }

    if (start_frame >= pmm64_max_frame) {
        return 0;
    }

    if (frame_count > pmm64_max_frame - start_frame) {
        return 0;
    }

    for (nq_u64 i = 0; i < frame_count; i++) {
        if (bitmap_test(start_frame + i)) {
            return 0;
        }
    }

    return 1;
}

nq_u64 pmm64_alloc_contiguous_frames(nq_u64 frame_count) {
    if (!pmm64_ready || frame_count == 0) {
        return 0;
    }

    if (frame_count > pmm64_max_frame) {
        return 0;
    }

    nq_u64 max_start = pmm64_max_frame - frame_count;

    for (nq_u64 pass = 0; pass < 2; pass++) {
        nq_u64 start = (pass == 0) ? pmm64_next_search : 0;
        nq_u64 end   = (pass == 0) ? max_start + 1 : pmm64_next_search;

        for (nq_u64 frame = start; frame < end; frame++) {
            if (!pmm64_frame_range_is_free(frame, frame_count)) {
                continue;
            }

            for (nq_u64 i = 0; i < frame_count; i++) {
                mark_frame_used(frame + i);
            }

            pmm64_next_search = frame + frame_count;

            return frame * PMM64_FRAME_SIZE;
        }
    }

    k64_print("PMM64: contiguous allocation failed. frames=");
    k64_print_dec64(frame_count);
    k64_print(" free_frames=");
    k64_print_dec64(pmm64_free_frames());
    k64_print("\n");

    return 0;
}

void pmm64_free_contiguous_frames(nq_u64 phys_addr, nq_u64 frame_count) {
    if (!pmm64_ready || frame_count == 0) {
        return;
    }

    if ((phys_addr & (PMM64_FRAME_SIZE - 1ull)) != 0) {
        return;
    }

    nq_u64 start_frame = phys_addr / PMM64_FRAME_SIZE;

    if (start_frame >= pmm64_max_frame) {
        return;
    }

    if (frame_count > pmm64_max_frame - start_frame) {
        return;
    }

    for (nq_u64 i = 0; i < frame_count; i++) {
        mark_frame_free(start_frame + i);
    }

    if (start_frame < pmm64_next_search) {
        pmm64_next_search = start_frame;
    }
}

nq_u64 pmm64_managed_frames(void) {
    return pmm64_max_frame;
}

nq_u64 pmm64_usable_frames(void) {
    return pmm64_usable_frame_count;
}

nq_u64 pmm64_free_frames(void) {
    nq_u64 free_count = 0;

    for (nq_u64 frame = 0; frame < pmm64_max_frame; frame++) {
        if (!bitmap_test(frame)) {
            free_count++;
        }
    }

    return free_count;
}

nq_u64 pmm64_used_frames(void) {
    nq_u64 free_count = pmm64_free_frames();

    if (free_count > pmm64_max_frame) {
        return 0;
    }

    return pmm64_max_frame - free_count;
}

nq_u64 pmm64_top_address(void) {
    return pmm64_max_frame * PMM64_FRAME_SIZE;
}

void pmm64_print_summary(void) {
    k64_print("PMM64 initialized.\n");

    k64_print("Managed memory top: ");
    k64_print_hex64(pmm64_top_address());
    k64_print("\n");

    k64_print("Managed frames: ");
    k64_print_dec64(pmm64_managed_frames());
    k64_print("\n");

    k64_print("UEFI usable frames: ");
    k64_print_dec64(pmm64_usable_frames());
    k64_print("\n");

    k64_print("Free frames: ");
    k64_print_dec64(pmm64_free_frames());
    k64_print("\n");

    k64_print("Used/reserved frames: ");
    k64_print_dec64(pmm64_used_frames());
    k64_print("\n");

    k64_print("Bitmap size: ");
    k64_print_dec64(sizeof(pmm64_bitmap));
    k64_print(" bytes\n");

    k64_print("Kernel early reserve: ");
    k64_print_dec64(PMM64_EARLY_KERNEL_RESERVE_SIZE);
    k64_print(" bytes\n");

    k64_print("POC user reserve: removed; user pages are allocated per-process.\n");
}
