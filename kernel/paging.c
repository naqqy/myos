#include "paging.h"
#include "pmm.h"
#include "graphics/framebuffer.h"

typedef unsigned int u32;
typedef unsigned char u8;

#define PAGE_SIZE 4096

static u32* kernel_page_directory = 0;
static u32* active_page_directory = 0;

static u32 align_down(u32 value, u32 align) {
    return value & ~(align - 1);
}

static int paging_is_enabled(void) {
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & 0x80000000u) != 0;
}

static void flush_tlb_all(void) {
    if (!active_page_directory) {
        return;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(active_page_directory) : "memory");
}

static u32 align_up(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}

static void mem_zero(void* ptr, u32 size) {
    u8* p = (u8*)ptr;
    for (u32 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void load_page_directory(u32* page_dir) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_dir) : "memory");
}

static void flush_tlb_single(u32 addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"((void*)addr) : "memory");
}

static void enable_paging_bit(void) {
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static u32* get_page_table(u32 virt_addr, int create) {
    u32 pd_index = virt_addr >> 22;

    if (!active_page_directory) {
        return 0;
    }

    if (!(active_page_directory[pd_index] & PAGE_PRESENT)) {
        if (!create) {
            return 0;
        }

        u32 pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            return 0;
        }

        /*
         * Current early paging model uses identity mapping for paging frames.
         */
        u32* pt = (u32*)pt_phys;
        mem_zero(pt, PAGE_SIZE);

        active_page_directory[pd_index] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        return pt;
    }

    return (u32*)(active_page_directory[pd_index] & 0xFFFFF000u);
}

void paging_map_page(u32 virt_addr, u32 phys_addr, u32 flags) {
    virt_addr = align_down(virt_addr, PAGE_SIZE);
    phys_addr = align_down(phys_addr, PAGE_SIZE);

    u32* pt = get_page_table(virt_addr, 1);
    if (!pt) {
        return;
    }

    u32 pt_index = (virt_addr >> 12) & 0x3FF;
    pt[pt_index] = phys_addr | (flags & 0xFFFu) | PAGE_PRESENT;

    /*
     * If paging is already enabled, reload CR3 instead of only invalidating
     * one page. This is important when mapping a framebuffer address because
     * creating a new page table also changes the page directory.
     */
    if (paging_is_enabled()) {
        flush_tlb_all();
    }
}

void paging_unmap_page(u32 virt_addr) {
    virt_addr = align_down(virt_addr, PAGE_SIZE);

    u32* pt = get_page_table(virt_addr, 0);
    if (!pt) {
        return;
    }

    u32 pt_index = (virt_addr >> 12) & 0x3FF;
    pt[pt_index] = 0;

    if (paging_is_enabled()) {
        flush_tlb_all();
    }
}

int paging_is_mapped(u32 virt_addr) {
    virt_addr = align_down(virt_addr, PAGE_SIZE);

    u32* pt = get_page_table(virt_addr, 0);
    if (!pt) {
        return 0;
    }

    u32 pt_index = (virt_addr >> 12) & 0x3FF;
    return (pt[pt_index] & PAGE_PRESENT) != 0;
}

u32 paging_get_physical(u32 virt_addr) {
    u32 page_base = align_down(virt_addr, PAGE_SIZE);
    u32* pt = get_page_table(page_base, 0);
    if (!pt) {
        return 0;
    }

    u32 pt_index = (page_base >> 12) & 0x3FF;
    u32 entry = pt[pt_index];

    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }

    return (entry & 0xFFFFF000u) | (virt_addr & 0xFFFu);
}

void paging_identity_map_range(u32 start, u32 length, u32 flags) {
    u32 addr = align_down(start, PAGE_SIZE);
    u32 end  = align_up(start + length, PAGE_SIZE);

    while (addr < end) {
        paging_map_page(addr, addr, flags);
        addr += PAGE_SIZE;
    }
}

u32* paging_kernel_directory(void) {
    return kernel_page_directory;
}

u32* paging_current_directory(void) {
    return active_page_directory;
}

void paging_switch_directory(u32* dir) {
    if (!dir) {
        return;
    }

    active_page_directory = dir;
    load_page_directory(dir);
}

void paging_switch_kernel_directory(void) {
    if (!kernel_page_directory) {
        return;
    }

    paging_switch_directory(kernel_page_directory);
}

u32* paging_create_process_directory(void) {
    if (!kernel_page_directory) {
        return 0;
    }

    u32 pd_phys = pmm_alloc_frame();
    if (!pd_phys) {
        return 0;
    }

    /*
     * Current paging model assumes low physical memory is identity mapped.
     */
    u32* dir = (u32*)pd_phys;
    mem_zero(dir, PAGE_SIZE);

    /*
     * Stage 1 address spaces:
     * Clone all current kernel mappings.
     *
     * This means process directories share the same page tables for now.
     * That is not isolation yet, but CR3 switching works and the process
     * has its own page directory object.
     */
    for (u32 i = 0; i < 1024; i++) {
        dir[i] = kernel_page_directory[i];
    }

    return dir;
}

void paging_destroy_process_directory(u32* dir) {
    if (!dir) {
        return;
    }

    if (dir == kernel_page_directory) {
        return;
    }

    /*
     * Stage 1:
     * Do not free page tables, because they are shared/cloned from kernel.
     * Only free the process page directory frame itself.
     */
    pmm_free_frame((u32)dir);
}

static void mem_copy_u32(u32* dst, const u32* src, u32 count) {
    for (u32 i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

static u32* get_page_table_in_directory(u32* dir, u32 virt_addr, int create) {
    u32 pd_index = virt_addr >> 22;

    if (!dir) {
        return 0;
    }

    if (!(dir[pd_index] & PAGE_PRESENT)) {
        if (!create) {
            return 0;
        }

        u32 pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            return 0;
        }

        u32* pt = (u32*)pt_phys;
        mem_zero(pt, PAGE_SIZE);

        dir[pd_index] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        return pt;
    }

    /*
     * If this process directory still points at the same page table as
     * the kernel directory, clone that table before modifying it.
     *
     * This prevents app mappings from overwriting the kernel/global
     * identity mappings for every address space.
     */
    if (dir != kernel_page_directory &&
        kernel_page_directory &&
        dir[pd_index] == kernel_page_directory[pd_index]) {

        u32 old_pt_phys = dir[pd_index] & 0xFFFFF000u;
        u32* old_pt = (u32*)old_pt_phys;

        u32 new_pt_phys = pmm_alloc_frame();
        if (!new_pt_phys) {
            return 0;
        }

        u32* new_pt = (u32*)new_pt_phys;
        mem_copy_u32(new_pt, old_pt, 1024);

        dir[pd_index] =
            new_pt_phys |
            (dir[pd_index] & 0xFFFu) |
            PAGE_PRESENT |
            PAGE_WRITABLE;

        return new_pt;
    }

    return (u32*)(dir[pd_index] & 0xFFFFF000u);
}

int paging_map_page_in_directory(u32* dir, u32 virt_addr, u32 phys_addr, u32 flags) {
    virt_addr = align_down(virt_addr, PAGE_SIZE);
    phys_addr = align_down(phys_addr, PAGE_SIZE);

    u32* pt = get_page_table_in_directory(dir, virt_addr, 1);
    if (!pt) {
        return 0;
    }

    u32 pd_index = virt_addr >> 22;
    u32 pt_index = (virt_addr >> 12) & 0x3FF;

    pt[pt_index] = phys_addr | (flags & 0xFFFu) | PAGE_PRESENT;

    /*
    * If this is user-accessible memory, both the PDE and PTE need PAGE_USER.
    * Otherwise ring 3 will page fault even if the PTE has PAGE_USER.
    */
    if (flags & PAGE_USER) {
        dir[pd_index] |= PAGE_USER;
    }

    if (dir == active_page_directory) {
        flush_tlb_single(virt_addr);
    }

    return 1;
}

int paging_unmap_page_in_directory(u32* dir, u32 virt_addr) {
    virt_addr = align_down(virt_addr, PAGE_SIZE);

    u32* pt = get_page_table_in_directory(dir, virt_addr, 0);
    if (!pt) {
        return 0;
    }

    u32 pt_index = (virt_addr >> 12) & 0x3FF;
    pt[pt_index] = 0;

    if (dir == active_page_directory) {
        flush_tlb_single(virt_addr);
    }

    return 1;
}

u32 paging_get_physical_in_directory(u32* dir, u32 virt_addr) {
    u32 page_base = align_down(virt_addr, PAGE_SIZE);

    u32* pt = get_page_table_in_directory(dir, page_base, 0);
    if (!pt) {
        return 0;
    }

    u32 pt_index = (page_base >> 12) & 0x3FF;
    u32 entry = pt[pt_index];

    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }

    return (entry & 0xFFFFF000u) | (virt_addr & 0xFFFu);
}

void paging_init(void) {

    u32 pd_phys = pmm_alloc_frame();
    if (!pd_phys) {
        return;
    }


    kernel_page_directory = (u32*)pd_phys;
    active_page_directory = kernel_page_directory;

    mem_zero(kernel_page_directory, PAGE_SIZE);

    u32 identity_map_size = pmm_top_address();
    if (identity_map_size == 0) {
        identity_map_size = 0x02000000u;
    }

    paging_identity_map_range(0x00000000, identity_map_size, PAGE_WRITABLE);

    if (fb_available()) {
        u32 fb_start = fb_phys_addr();
        u32 fb_size  = fb_size_bytes();

        if (fb_start != 0 &&
            fb_size != 0 &&
            fb_size <= (16u * 1024u * 1024u) &&
            fb_start >= 0x01000000u &&
            fb_start + fb_size > fb_start) {
            paging_identity_map_range(fb_start, fb_size, PAGE_WRITABLE);
        }
    }

    load_page_directory(kernel_page_directory);

    enable_paging_bit();
}