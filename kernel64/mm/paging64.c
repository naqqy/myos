#include "paging64.h"
#include "pmm64.h"
#include "../k64_console.h"
#include "vmem64.h"

#define PTE_ADDR_MASK_4K 0x000FFFFFFFFFF000ull
#define PTE_ADDR_MASK_2M 0x000FFFFFFFE00000ull

#define CANONICAL_LOW_LIMIT 0x0000800000000000ull

#define PAGING64_STATIC_TABLE_PAGES 64u
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

    k64_print("paging64: direct map enabled\n");

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
    return (nq_u64*)(entry & PAGE64_ADDR_MASK);
}

int paging64_mark_user_range(nq_u64 virt_addr, nq_u64 size) {
    if (!g_pml4 || size == 0) {
        return 0;
    }

    nq_u64 start = virt_addr & ~(PAGE64_2M_SIZE - 1ull);
    nq_u64 end = virt_addr + size;

    if (end < virt_addr) {
        return 0;
    }

    end = (end + PAGE64_2M_SIZE - 1ull) & ~(PAGE64_2M_SIZE - 1ull);

    for (nq_u64 addr = start; addr < end; addr += PAGE64_2M_SIZE) {
        nq_u64 pml4_i = (addr >> 39) & 0x1FFull;
        nq_u64 pdpt_i = (addr >> 30) & 0x1FFull;
        nq_u64 pd_i   = (addr >> 21) & 0x1FFull;

        nq_u64* pml4 = g_pml4;

        if (!(pml4[pml4_i] & PAGE64_PRESENT)) {
            return 0;
        }

        nq_u64* pdpt = paging64_table_from_entry(pml4[pml4_i]);

        if (!(pdpt[pdpt_i] & PAGE64_PRESENT)) {
            return 0;
        }

        nq_u64* pd = paging64_table_from_entry(pdpt[pdpt_i]);

        if (!(pd[pd_i] & PAGE64_PRESENT)) {
            return 0;
        }

        if (!(pd[pd_i] & PAGE64_PS)) {
            return 0;
        }

        /*
         * User access requires USER bit on every paging level.
         */
        pml4[pml4_i] |= PAGE64_USER;
        pdpt[pdpt_i] |= PAGE64_USER;
        pd[pd_i]     |= PAGE64_USER | PAGE64_RW;
    }

    /*
     * Flush TLB.
     */
    __asm__ volatile (
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        :
        :
        : "rax", "memory"
    );

    return 1;
}

static nq_u64* paging64_table_from_entry_checked(nq_u64 entry) {
    return (nq_u64*)(entry & PAGE64_ADDR_MASK);
}

static int paging64_user_addr_accessible(nq_u64 addr) {
    if (!g_pml4) {
        return 0;
    }

    nq_u64 pml4_i = (addr >> 39) & 0x1FFull;
    nq_u64 pdpt_i = (addr >> 30) & 0x1FFull;
    nq_u64 pd_i   = (addr >> 21) & 0x1FFull;

    nq_u64 pml4e = g_pml4[pml4_i];

    if (!(pml4e & PAGE64_PRESENT) || !(pml4e & PAGE64_USER)) {
        return 0;
    }

    nq_u64* pdpt = paging64_table_from_entry_checked(pml4e);
    nq_u64 pdpte = pdpt[pdpt_i];

    if (!(pdpte & PAGE64_PRESENT) || !(pdpte & PAGE64_USER)) {
        return 0;
    }

    nq_u64* pd = paging64_table_from_entry_checked(pdpte);
    nq_u64 pde = pd[pd_i];

    if (!(pde & PAGE64_PRESENT) || !(pde & PAGE64_USER)) {
        return 0;
    }

    /*
     * Current paging64 uses 2 MiB pages.
     * If this is not a 2 MiB page, reject for now.
     */
    if (!(pde & PAGE64_PS)) {
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

    /*
     * Overflow check.
     */
    nq_u64 end = virt_addr + size - 1u;

    if (end < virt_addr) {
        return 0;
    }

    /*
     * Check first and last byte, then each crossed 2 MiB boundary.
     */
    if (!paging64_user_addr_accessible(virt_addr)) {
        return 0;
    }

    if (!paging64_user_addr_accessible(end)) {
        return 0;
    }

    nq_u64 page = (virt_addr & ~(PAGE64_2M_SIZE - 1ull)) + PAGE64_2M_SIZE;

    while (page < end) {
        if (!paging64_user_addr_accessible(page)) {
            return 0;
        }

        page += PAGE64_2M_SIZE;
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