#ifndef PAGING64_H
#define PAGING64_H

#include "naq_boot_info.h"

#define PAGE64_PRESENT  0x001ull
#define PAGE64_WRITABLE 0x002ull
#define PAGE64_USER     0x004ull
#define PAGE64_HUGE     0x080ull

#define PAGE64_SIZE_4K  4096ull
#define PAGE64_SIZE_2M  0x200000ull

int paging64_init(const naq_boot_info_t* boot);

int paging64_identity_map_range(nq_u64 base, nq_u64 length, nq_u64 flags);

nq_u64 paging64_kernel_pml4(void);
nq_u64 paging64_current_pml4(void);
void   paging64_switch_pml4(nq_u64 pml4_phys);

nq_u64 paging64_create_user_address_space(void);
void   paging64_destroy_user_address_space(nq_u64 pml4_phys);

int paging64_map_user_page(
    nq_u64 pml4_phys,
    nq_u64 virt,
    nq_u64 phys,
    nq_u64 flags
);

int paging64_alloc_map_user_range(
    nq_u64 pml4_phys,
    nq_u64 virt,
    nq_u64 size,
    nq_u64 flags
);

nq_u64 paging64_mapped_2m_pages(void);
nq_u64 paging64_table_pages(void);

int paging64_user_range_accessible(nq_u64 virt_addr, nq_u64 size);

void paging64_print_summary(void);

int paging64_map_2m_range(
    nq_u64 virt,
    nq_u64 phys,
    nq_u64 size,
    nq_u64 flags
);

int paging64_map_direct_range(nq_u64 phys_start, nq_u64 size);

void paging64_print_layout(void);

/*
 * 1 if the framebuffer is mapped write-combined (fast screen updates).
 */
int paging64_framebuffer_write_combined(void);

#endif