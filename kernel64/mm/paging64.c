#include "paging64.h"
#include "pmm64.h"
#include "../k64_console.h"
#include "vmem64.h"
#include "../debug/bootlog64.h"

#define PTE_ADDR_MASK_4K 0x000FFFFFFFFFF000ull
#define PTE_ADDR_MASK_2M 0x000FFFFFFFE00000ull

#define CANONICAL_LOW_LIMIT 0x0000800000000000ull

#define PAGING64_STATIC_TABLE_PAGES 256u
#define PAGING64_STATIC_TABLE_BYTES (PAGING64_STATIC_TABLE_PAGES * PAGE64_SIZE_4K)

#define PAGE64_PRESENT  0x001ull
#define PAGE64_RW       0x002ull
#define PAGE64_USER     0x004ull
#define PAGE64_PS       0x080ull

#define PAGE64_ADDR_MASK 0x000FFFFFFFFFF000ull
#define PAGE64_2M_MASK   0x000FFFFFFFE00000ull
#define PAGE64_2M_SIZE   0x200000ull
#define PAGE64_NX        0x8000000000000000ull


static nq_u64* g_pml4 = 0;
static nq_u64  g_pml4_phys = 0;

static nq_u64 g_mapped_2m_pages = 0;
static nq_u64 g_table_pages = 0;

/*
 * Early page-table pool.
 *
 * Important:
 * Do not allocate page tables from PMM64 yet. PMM64 may return physical
 * frames that are not mapped by the current UEFI page tables before we switch
 * CR3. This static pool lives inside the loaded kernel image, so it is
 * accessible before and after the CR3 switch.
 *
 * The nonzero initializer forces this into the binary/data image instead of
 * relying on uninitialized BSS behavior during early boot.
 */
static nq_u8 paging64_table_pool[PAGING64_STATIC_TABLE_BYTES]
    __attribute__((aligned(4096), section(".data.paging64_pool"))) = { 1 };

static nq_u64 paging64_table_pool_used = 0;

static nq_u64 align_down64_local(nq_u64 value, nq_u64 align) {
    return value & ~(align - 1ull);
}

static nq_u64 align_up64_local(nq_u64 value, nq_u64 align) {
    return (value + align - 1ull) & ~(align - 1ull);
}

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

static int is_low_canonical(nq_u64 addr) {
    return addr < CANONICAL_LOW_LIMIT;
}

static void mem_zero64(void* ptr, nq_u64 bytes) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < bytes; i++) {
        p[i] = 0;
    }
}

static nq_u64 read_cr3(void) {
    nq_u64 value;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));

    return value;
}

static void write_cr3(nq_u64 value) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(value) : "memory");
}

static nq_u64* alloc_table_page(void) {
    if (paging64_table_pool_used >= PAGING64_STATIC_TABLE_PAGES) {
        k64_print("Paging64: static table pool exhausted.\n");
        return 0;
    }

    nq_u64 offset = paging64_table_pool_used * PAGE64_SIZE_4K;
    nq_u64* table = (nq_u64*)(paging64_table_pool + offset);

    paging64_table_pool_used++;

    mem_zero64(table, PAGE64_SIZE_4K);

    g_table_pages++;

    return table;
}

static nq_u64 paging64_current_pml4_phys_raw(void) {
    return read_cr3() & PAGE64_ADDR_MASK;
}

nq_u64 paging64_current_pml4(void) {
    return paging64_current_pml4_phys_raw();
}

void paging64_switch_pml4(nq_u64 pml4_phys) {
    if (!pml4_phys) {
        pml4_phys = g_pml4_phys;
    }

    write_cr3(pml4_phys & PAGE64_ADDR_MASK);
}

static nq_u64* paging64_phys_table_ptr(nq_u64 phys) {
    if (!phys) {
        return 0;
    }

    return (nq_u64*)vmem64_phys_to_ptr(phys & PAGE64_ADDR_MASK);
}

static nq_u64 paging64_alloc_runtime_table_phys(void) {
    nq_u64 phys = pmm64_alloc_frame();

    if (!phys) {
        k64_print("paging64: failed to allocate runtime table page\n");
        return 0;
    }

    nq_u64* ptr = paging64_phys_table_ptr(phys);

    if (!ptr) {
        pmm64_free_frame(phys);
        return 0;
    }

    mem_zero64(ptr, PAGE64_SIZE_4K);
    g_table_pages++;

    return phys;
}

static void paging64_copy_table(nq_u64* dst, const nq_u64* src) {
    if (!dst || !src) {
        return;
    }

    for (nq_u32 i = 0; i < 512; i++) {
        dst[i] = src[i];
    }
}




nq_u64 paging64_create_user_address_space(void) {
    if (!g_pml4 || !g_pml4_phys) {
        return 0;
    }

    /*
     * The user slot must be free in the kernel's own tables, otherwise
     * processes would share (and free) kernel page tables.
     */
    if (g_pml4[VMEM64_USER_PML4_INDEX] & PAGE64_PRESENT) {
        k64_print("paging64: user PML4 slot is in use by the kernel\n");
        return 0;
    }

    nq_u64 pml4_phys = paging64_alloc_runtime_table_phys();

    if (!pml4_phys) {
        return 0;
    }

    nq_u64* pml4 = paging64_phys_table_ptr(pml4_phys);

    if (!pml4) {
        pmm64_free_frame(pml4_phys);
        return 0;
    }

    /*
     * Every kernel entry (identity map, direct map, ...) is shared: the new
     * PML4 points at the kernel's own lower-level tables, so kernel mappings
     * stay identical in every process and stay supervisor-only.
     *
     * Only the user slot is private. It starts empty and is filled by
     * paging64_map_user_page().
     */
    paging64_copy_table(pml4, g_pml4);
    pml4[VMEM64_USER_PML4_INDEX] = 0;

    return pml4_phys;
}

static int paging64_get_or_alloc_table_for_user(
    nq_u64* table,
    nq_u64 index,
    nq_u64 entry_flags,
    nq_u64* out_phys,
    nq_u64** out_table
) {
    if (!table || index >= 512 || !out_phys || !out_table) {
        return 0;
    }

    nq_u64 entry = table[index];

    if (entry & PAGE64_PRESENT) {
        nq_u64 phys = entry & PAGE64_ADDR_MASK;
        nq_u64* next = paging64_phys_table_ptr(phys);

        if (!next) {
            return 0;
        }

        table[index] |= entry_flags;

        *out_phys = phys;
        *out_table = next;
        return 1;
    }

    nq_u64 phys = paging64_alloc_runtime_table_phys();

    if (!phys) {
        return 0;
    }

    table[index] =
        phys |
        PAGE64_PRESENT |
        PAGE64_RW |
        entry_flags;

    *out_phys = phys;
    *out_table = paging64_phys_table_ptr(phys);

    return *out_table != 0;
}

int paging64_map_user_page(
    nq_u64 pml4_phys,
    nq_u64 virt,
    nq_u64 phys,
    nq_u64 flags
) {
    if (!pml4_phys || !phys) {
        return 0;
    }

    if ((virt & (PAGE64_SIZE_4K - 1ull)) ||
        (phys & (PAGE64_SIZE_4K - 1ull))) {
        return 0;
    }

    /*
     * User pages may only be created inside the user region. This is what
     * keeps USER-accessible entries out of the shared kernel tables.
     */
    if (!vmem64_is_user(virt)) {
        return 0;
    }

    if (pml4_phys == g_pml4_phys) {
        return 0;
    }

    nq_u64* pml4 = paging64_phys_table_ptr(pml4_phys);

    if (!pml4) {
        return 0;
    }

    nq_u64 pml4_i = (virt >> 39) & 0x1FFull;
    nq_u64 pdpt_i = (virt >> 30) & 0x1FFull;
    nq_u64 pd_i   = (virt >> 21) & 0x1FFull;
    nq_u64 pt_i   = (virt >> 12) & 0x1FFull;

    nq_u64 pdpt_phys = 0;
    nq_u64 pd_phys = 0;
    nq_u64 pt_phys = 0;

    nq_u64* pdpt = 0;
    nq_u64* pd = 0;
    nq_u64* pt = 0;

    if (!paging64_get_or_alloc_table_for_user(
            pml4,
            pml4_i,
            PAGE64_USER,
            &pdpt_phys,
            &pdpt)) {
        return 0;
    }

    if (!paging64_get_or_alloc_table_for_user(
            pdpt,
            pdpt_i,
            PAGE64_USER,
            &pd_phys,
            &pd)) {
        return 0;
    }

    nq_u64 pde = pd[pd_i];

    if (pde & PAGE64_PRESENT) {
        if (pde & PAGE64_PS) {
            /*
             * The user slot only ever holds 4 KiB pages created here.
             */
            return 0;
        } else {
            pt_phys = pde & PAGE64_ADDR_MASK;
            pt = paging64_phys_table_ptr(pt_phys);

            if (!pt) {
                return 0;
            }

            pd[pd_i] |= PAGE64_USER | PAGE64_RW;
        }
    } else {
        pt_phys = paging64_alloc_runtime_table_phys();

        if (!pt_phys) {
            return 0;
        }

        pt = paging64_phys_table_ptr(pt_phys);

        if (!pt) {
            pmm64_free_frame(pt_phys);
            return 0;
        }

        pd[pd_i] =
            pt_phys |
            PAGE64_PRESENT |
            PAGE64_RW |
            PAGE64_USER;
    }

    pt[pt_i] =
        (phys & PAGE64_ADDR_MASK) |
        PAGE64_PRESENT |
        PAGE64_USER |
        PAGE64_RW |
        (flags & 0xFFFu);

    if ((read_cr3() & PAGE64_ADDR_MASK) == (pml4_phys & PAGE64_ADDR_MASK)) {
        write_cr3(pml4_phys & PAGE64_ADDR_MASK);
    }

    return 1;
}

/*
 * Returns 1 if virt already has a private 4 KiB user page in this address
 * space (not a cloned 2 MiB identity mapping).
 */
static int paging64_user_page_is_mapped(nq_u64 pml4_phys, nq_u64 virt) {
    nq_u64* pml4 = paging64_phys_table_ptr(pml4_phys);

    if (!pml4) {
        return 0;
    }

    nq_u64 pml4e = pml4[(virt >> 39) & 0x1FFull];

    if (!(pml4e & PAGE64_PRESENT)) {
        return 0;
    }

    nq_u64* pdpt = paging64_phys_table_ptr(pml4e & PAGE64_ADDR_MASK);

    if (!pdpt) {
        return 0;
    }

    nq_u64 pdpte = pdpt[(virt >> 30) & 0x1FFull];

    if (!(pdpte & PAGE64_PRESENT) || (pdpte & PAGE64_PS)) {
        return 0;
    }

    nq_u64* pd = paging64_phys_table_ptr(pdpte & PAGE64_ADDR_MASK);

    if (!pd) {
        return 0;
    }

    nq_u64 pde = pd[(virt >> 21) & 0x1FFull];

    if (!(pde & PAGE64_PRESENT) || (pde & PAGE64_PS)) {
        return 0;
    }

    nq_u64* pt = paging64_phys_table_ptr(pde & PAGE64_ADDR_MASK);

    if (!pt) {
        return 0;
    }

    nq_u64 pte = pt[(virt >> 12) & 0x1FFull];

    return (pte & PAGE64_PRESENT) && (pte & PAGE64_USER);
}

int paging64_alloc_map_user_range(
    nq_u64 pml4_phys,
    nq_u64 virt,
    nq_u64 size,
    nq_u64 flags
) {
    if (!pml4_phys || size == 0) {
        return 0;
    }

    nq_u64 start = align_down64(virt, PAGE64_SIZE_4K);
    nq_u64 end = align_up64(virt + size, PAGE64_SIZE_4K);

    if (end < start) {
        return 0;
    }

    for (nq_u64 va = start; va < end; va += PAGE64_SIZE_4K) {
        /*
         * Two ELF segments may share a page. Keep the page that is already
         * there instead of replacing it with a fresh zeroed frame, which
         * would wipe the first segment's data and leak its frame.
         */
        if (paging64_user_page_is_mapped(pml4_phys, va)) {
            continue;
        }

        nq_u64 phys = pmm64_alloc_frame();

        if (!phys) {
            k64_print("paging64: failed to allocate user frame\n");
            return 0;
        }

        void* ptr = vmem64_phys_to_ptr(phys);

        if (!ptr) {
            pmm64_free_frame(phys);
            return 0;
        }

        mem_zero64(ptr, PAGE64_SIZE_4K);

        if (!paging64_map_user_page(
                pml4_phys,
                va,
                phys,
                flags | PAGE64_USER | PAGE64_RW)) {
            pmm64_free_frame(phys);
            return 0;
        }
    }

    return 1;
}

/*
 * Frees everything under the private user PML4 slot: the user frames, then
 * the page tables, page directories and the PDPT that map them.
 */
static void paging64_free_user_slot(nq_u64 pml4_phys) {
    nq_u64* pml4 = paging64_phys_table_ptr(pml4_phys);

    if (!pml4) {
        return;
    }

    nq_u64 pml4e = pml4[VMEM64_USER_PML4_INDEX];

    if (!(pml4e & PAGE64_PRESENT)) {
        return;
    }

    nq_u64 pdpt_phys = pml4e & PAGE64_ADDR_MASK;
    nq_u64* pdpt = paging64_phys_table_ptr(pdpt_phys);

    pml4[VMEM64_USER_PML4_INDEX] = 0;

    if (!pdpt) {
        return;
    }

    for (nq_u32 pdpt_i = 0; pdpt_i < 512; pdpt_i++) {
        nq_u64 pdpte = pdpt[pdpt_i];

        if (!(pdpte & PAGE64_PRESENT) || (pdpte & PAGE64_PS)) {
            continue;
        }

        nq_u64 pd_phys = pdpte & PAGE64_ADDR_MASK;
        nq_u64* pd = paging64_phys_table_ptr(pd_phys);

        if (!pd) {
            continue;
        }

        for (nq_u32 pd_i = 0; pd_i < 512; pd_i++) {
            nq_u64 pde = pd[pd_i];

            if (!(pde & PAGE64_PRESENT) || (pde & PAGE64_PS)) {
                continue;
            }

            nq_u64 pt_phys = pde & PAGE64_ADDR_MASK;
            nq_u64* pt = paging64_phys_table_ptr(pt_phys);

            if (pt) {
                for (nq_u32 pt_i = 0; pt_i < 512; pt_i++) {
                    nq_u64 pte = pt[pt_i];

                    if (pte & PAGE64_PRESENT) {
                        pmm64_free_frame(pte & PAGE64_ADDR_MASK);
                    }
                }
            }

            pmm64_free_frame(pt_phys);
        }

        pmm64_free_frame(pd_phys);
    }

    pmm64_free_frame(pdpt_phys);
}

void paging64_destroy_user_address_space(nq_u64 pml4_phys) {
    if (!pml4_phys || pml4_phys == g_pml4_phys) {
        return;
    }

    if ((read_cr3() & PAGE64_ADDR_MASK) == (pml4_phys & PAGE64_ADDR_MASK)) {
        write_cr3(g_pml4_phys);
    }

    paging64_free_user_slot(pml4_phys);
    pmm64_free_frame(pml4_phys);
}

int paging64_map_2m_range(
    nq_u64 virt,
    nq_u64 phys,
    nq_u64 size,
    nq_u64 flags
) {
    if (!g_pml4 || size == 0) {
        return 0;
    }

    nq_u64 virt_start = align_down64_local(virt, PAGE64_2M_SIZE);
    nq_u64 phys_start = align_down64_local(phys, PAGE64_2M_SIZE);
    nq_u64 virt_end = align_up64_local(virt + size, PAGE64_2M_SIZE);

    if (virt_end < virt_start) {
        return 0;
    }

    for (nq_u64 va = virt_start, pa = phys_start;
         va < virt_end;
         va += PAGE64_2M_SIZE, pa += PAGE64_2M_SIZE) {
        nq_u64 pml4_i = (va >> 39) & 0x1FFull;
        nq_u64 pdpt_i = (va >> 30) & 0x1FFull;
        nq_u64 pd_i   = (va >> 21) & 0x1FFull;

        nq_u64* pdpt;
        nq_u64* pd;

        if (!(g_pml4[pml4_i] & PAGE64_PRESENT)) {
            pdpt = alloc_table_page();

            if (!pdpt) {
                return 0;
            }

            g_pml4[pml4_i] =
                ((nq_u64)pdpt & PAGE64_ADDR_MASK) |
                PAGE64_PRESENT |
                PAGE64_RW;
        } else {
            pdpt = (nq_u64*)(g_pml4[pml4_i] & PAGE64_ADDR_MASK);
        }

        if (!(pdpt[pdpt_i] & PAGE64_PRESENT)) {
            pd = alloc_table_page();

            if (!pd) {
                return 0;
            }

            pdpt[pdpt_i] =
                ((nq_u64)pd & PAGE64_ADDR_MASK) |
                PAGE64_PRESENT |
                PAGE64_RW;
        } else {
            pd = (nq_u64*)(pdpt[pdpt_i] & PAGE64_ADDR_MASK);
        }

        /*
         * If mapping user pages, USER must exist on every level.
         */
        if (flags & PAGE64_USER) {
            g_pml4[pml4_i] |= PAGE64_USER;
            pdpt[pdpt_i]   |= PAGE64_USER;
        }

        pd[pd_i] =
            (pa & PAGE64_2M_MASK) |
            PAGE64_PRESENT |
            PAGE64_PS |
            flags;
    }

    __asm__ volatile (
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        :
        :
        : "rax", "memory"
    );

    return 1;
}

int paging64_map_direct_range(nq_u64 phys_start, nq_u64 size) {
    nq_u64 phys = align_down64_local(phys_start, PAGE64_2M_SIZE);
    nq_u64 end = align_up64_local(phys_start + size, PAGE64_2M_SIZE);

    if (end < phys) {
        return 0;
    }

    if ((end - phys) > VMEM64_DIRECT_MAP_SIZE) {
        k64_print("paging64: direct map range too large\n");
        return 0;
    }

    return paging64_map_2m_range(
        VMEM64_DIRECT_MAP_BASE + phys,
        phys,
        end - phys,
        PAGE64_RW
    );
}

void paging64_print_layout(void) {
    k64_print("Paging64:\n");

    k64_print("  2MiB identity map: enabled temporary\n");
    k64_print("  direct map base: ");
    k64_print_hex64(VMEM64_DIRECT_MAP_BASE);
    k64_print("\n");

    k64_print("  table pool pages: ");
    k64_print_dec64(PAGING64_STATIC_TABLE_PAGES);
    k64_print(" used=");
    k64_print_dec64(paging64_table_pool_used);
    k64_print("\n");
}

static nq_u64* get_or_create_next_table(nq_u64* table, nq_u64 index) {
    nq_u64 entry;

    if (!table || index >= 512) {
        return 0;
    }

    entry = table[index];

    if (entry & PAGE64_PRESENT) {
        return (nq_u64*)(entry & PTE_ADDR_MASK_4K);
    }

    nq_u64* next = alloc_table_page();

    if (!next) {
        return 0;
    }

    table[index] =
        ((nq_u64)next & PTE_ADDR_MASK_4K) |
        PAGE64_PRESENT |
        PAGE64_WRITABLE;

    return next;
}

static int map_2m_page(nq_u64 virt, nq_u64 phys, nq_u64 flags) {
    if (!g_pml4) {
        return 0;
    }

    if (!is_low_canonical(virt)) {
        return 0;
    }

    nq_u64 pml4_index = (virt >> 39) & 0x1FFull;
    nq_u64 pdpt_index = (virt >> 30) & 0x1FFull;
    nq_u64 pd_index   = (virt >> 21) & 0x1FFull;

    nq_u64* pdpt = get_or_create_next_table(g_pml4, pml4_index);
    if (!pdpt) {
        return 0;
    }

    nq_u64* pd = get_or_create_next_table(pdpt, pdpt_index);
    if (!pd) {
        return 0;
    }

    nq_u64 old = pd[pd_index];

    pd[pd_index] =
        (phys & PTE_ADDR_MASK_2M) |
        (flags & 0xFFFu) |
        PAGE64_PRESENT |
        PAGE64_HUGE;

    if (!(old & PAGE64_PRESENT)) {
        g_mapped_2m_pages++;
    }

    return 1;
}

int paging64_identity_map_range(nq_u64 base, nq_u64 length, nq_u64 flags) {
    nq_u64 raw_end;

    if (!checked_end64(base, length, &raw_end)) {
        return 0;
    }

    nq_u64 start = align_down64(base, PAGE64_SIZE_2M);
    nq_u64 end   = align_up64(raw_end, PAGE64_SIZE_2M);

    if (!is_low_canonical(start)) {
        return 0;
    }

    if (!is_low_canonical(end - 1)) {
        return 0;
    }

    for (nq_u64 addr = start; addr < end; addr += PAGE64_SIZE_2M) {
        if (!map_2m_page(addr, addr, flags)) {
            return 0;
        }
    }

    return 1;
}

static int map_boot_critical_regions(const naq_boot_info_t* boot) {
    /*
     * Map all managed physical memory identity.
     * This keeps the early kernel simple.
     */
    if (!paging64_identity_map_range(
            0,
            pmm64_top_address(),
            PAGE64_WRITABLE)) {
        return 0;
    }

    /*
     * Map kernel image explicitly.
     */
    if (!paging64_identity_map_range(
            boot->kernel_load_addr,
            boot->kernel_size,
            PAGE64_WRITABLE)) {
        return 0;
    }

    /*
     * Map kernel stack.
     */
    if (!paging64_identity_map_range(
            boot->stack_base,
            boot->stack_size,
            PAGE64_WRITABLE)) {
        return 0;
    }

    /*
     * Map boot info struct.
     */
    if (!paging64_identity_map_range(
            (nq_u64)boot,
            sizeof(*boot),
            PAGE64_WRITABLE)) {
        return 0;
    }

    /*
     * Map UEFI memory map buffer.
     */
    if (!paging64_identity_map_range(
            boot->memory_map,
            boot->memory_map_size,
            PAGE64_WRITABLE)) {
        return 0;
    }

    /*
     * Map GOP framebuffer.
     */
    if (!paging64_identity_map_range(
            boot->framebuffer_base,
            boot->framebuffer_size,
            PAGE64_WRITABLE)) {
        return 0;
    }

    return 1;
}

int paging64_init(const naq_boot_info_t* boot) {
    if (!boot ||
        boot->magic != NAQ_BOOT_INFO_MAGIC ||
        boot->version != NAQ_BOOT_INFO_VERSION) {
        return 0;
    }

    g_mapped_2m_pages = 0;
    g_table_pages = 0;
    paging64_table_pool_used = 0;

    g_pml4 = alloc_table_page();

    if (!g_pml4) {
        return 0;
    }

    g_pml4_phys = (nq_u64)g_pml4;


    if (!map_boot_critical_regions(boot)) {
        return 0;
    }

    /*
    * Phase 1 cleaner VM:
    * Keep identity map for compatibility, but also map a higher-half
    * direct map so new code can stop treating physical addresses as pointers.
    */
    if (!paging64_map_direct_range(0, VMEM64_DIRECT_MAP_SIZE)) {
        k64_print("paging64: direct map failed\n");
        return 0;
    }

    bootlog64_print("paging64: direct map enabled\n");

    write_cr3(g_pml4_phys);

    return 1;
}

nq_u64 paging64_kernel_pml4(void) {
    return g_pml4_phys;
}

nq_u64 paging64_mapped_2m_pages(void) {
    return g_mapped_2m_pages;
}

nq_u64 paging64_table_pages(void) {
    return g_table_pages;
}

static nq_u64* paging64_table_from_entry(nq_u64 entry) {
    return paging64_phys_table_ptr(entry & PAGE64_ADDR_MASK);
}


static nq_u64* paging64_table_from_entry_checked(nq_u64 entry) {
    return paging64_phys_table_ptr(entry & PAGE64_ADDR_MASK);
}


static int paging64_user_addr_accessible(nq_u64 addr) {
    /*
     * A user pointer must point into the user region. Kernel addresses are
     * rejected outright, whatever the page tables say.
     */
    if (!vmem64_is_user(addr)) {
        return 0;
    }

    nq_u64 cr3 = read_cr3() & PAGE64_ADDR_MASK;

    if (!cr3) {
        return 0;
    }

    nq_u64* pml4 = paging64_phys_table_ptr(cr3);

    if (!pml4) {
        return 0;
    }

    nq_u64 pml4_i = (addr >> 39) & 0x1FFull;
    nq_u64 pdpt_i = (addr >> 30) & 0x1FFull;
    nq_u64 pd_i   = (addr >> 21) & 0x1FFull;
    nq_u64 pt_i   = (addr >> 12) & 0x1FFull;

    nq_u64 pml4e = pml4[pml4_i];

    if (!(pml4e & PAGE64_PRESENT) || !(pml4e & PAGE64_USER)) {
        return 0;
    }

    nq_u64* pdpt = paging64_phys_table_ptr(pml4e & PAGE64_ADDR_MASK);

    if (!pdpt) {
        return 0;
    }

    nq_u64 pdpte = pdpt[pdpt_i];

    if (!(pdpte & PAGE64_PRESENT) || !(pdpte & PAGE64_USER)) {
        return 0;
    }

    nq_u64* pd = paging64_phys_table_ptr(pdpte & PAGE64_ADDR_MASK);

    if (!pd) {
        return 0;
    }

    nq_u64 pde = pd[pd_i];

    if (!(pde & PAGE64_PRESENT) || !(pde & PAGE64_USER)) {
        return 0;
    }

    if (pde & PAGE64_PS) {
        return 1;
    }

    nq_u64* pt = paging64_phys_table_ptr(pde & PAGE64_ADDR_MASK);

    if (!pt) {
        return 0;
    }

    nq_u64 pte = pt[pt_i];

    if (!(pte & PAGE64_PRESENT) || !(pte & PAGE64_USER)) {
        return 0;
    }

    return 1;
}


int paging64_user_range_accessible(nq_u64 virt_addr, nq_u64 size) {
    if (size == 0) {
        return 1;
    }

    if (virt_addr == 0) {
        return 0;
    }

    nq_u64 end = virt_addr + size - 1u;

    if (end < virt_addr) {
        return 0;
    }

    if (!paging64_user_addr_accessible(virt_addr)) {
        return 0;
    }

    if (!paging64_user_addr_accessible(end)) {
        return 0;
    }

    nq_u64 page = (virt_addr & ~(PAGE64_SIZE_4K - 1ull)) + PAGE64_SIZE_4K;

    while (page < end) {
        if (!paging64_user_addr_accessible(page)) {
            return 0;
        }

        page += PAGE64_SIZE_4K;
    }

    return 1;
}


void paging64_print_summary(void) {
    k64_print("Paging64 initialized.\n");

    k64_print("Old CR3/current CR3: ");
    k64_print_hex64(read_cr3());
    k64_print("\n");

    k64_print("Kernel PML4: ");
    k64_print_hex64(g_pml4_phys);
    k64_print("\n");

    k64_print("Page-table pages: ");
    k64_print_dec64(g_table_pages);
    k64_print("\n");

    k64_print("Mapped 2MiB pages: ");
    k64_print_dec64(g_mapped_2m_pages);
    k64_print("\n");

    k64_print("Identity mapped approx: ");
    k64_print_dec64(g_mapped_2m_pages * 2ull);
    k64_print(" MiB\n");
}