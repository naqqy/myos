#ifndef VMEM64_H
#define VMEM64_H

#include "naq_boot_info.h"

/*
 * NAQ OS kernel64 virtual memory layout.
 *
 * Phase 1:
 *   - Keep low identity map for compatibility.
 *   - Add higher-half/direct-map helpers.
 *
 * Later:
 *   - Move kernel heap to VMEM64_KERNEL_HEAP_BASE.
 *   - Move kernel image higher-half.
 *   - Remove broad low identity mapping.
 */

/*
 * User space.
 *
 * User programs live in their own top-level page-table slot (PML4 entry 1,
 * starting at 512 GiB). Nothing of the kernel is mapped there, and nothing
 * of a user program is mapped in the kernel's slots, so:
 *   - each process gets a private PML4[1] subtree; every other PML4 entry
 *     is shared with the kernel and stays supervisor-only,
 *   - user mappings can never shadow kernel identity-mapped memory,
 *   - a user pointer is valid only if it lies inside this range.
 *
 * userapps/userapp.ld links apps at VMEM64_USER_BASE.
 */
#define VMEM64_USER_BASE              0x0000008000000000ull
#define VMEM64_USER_LIMIT             0x0000008040000000ull  /* 1 GiB */
#define VMEM64_USER_PML4_INDEX        ((VMEM64_USER_BASE >> 39) & 0x1FFull)

/*
 * Low identity map.
 * Temporary compatibility region.
 */
#define VMEM64_IDENTITY_BASE          0x0000000000000000ull
#define VMEM64_IDENTITY_SIZE          0x0000000040000000ull  /* 1 GiB */

/*
 * Higher-half direct map.
 * Virtual = VMEM64_DIRECT_MAP_BASE + physical.
 *
 * This gives the kernel a clean way to access physical frames without
 * pretending physical addresses are normal kernel pointers.
 */
#define VMEM64_DIRECT_MAP_BASE        0xFFFF800000000000ull
#define VMEM64_DIRECT_MAP_SIZE        0x0000000040000000ull  /* 1 GiB for now */

/*
 * Future higher-half kernel regions.
 */
#define VMEM64_KERNEL_BASE            0xFFFFFFFF80000000ull
#define VMEM64_KERNEL_HEAP_BASE       0xFFFF900000000000ull
#define VMEM64_KERNEL_HEAP_SIZE       0x0000000010000000ull  /* 256 MiB */

#define VMEM64_PAGE_SIZE              0x1000ull
#define VMEM64_LARGE_PAGE_SIZE        0x200000ull

nq_u64 vmem64_phys_to_direct(nq_u64 phys);
nq_u64 vmem64_direct_to_phys(nq_u64 virt);

void* vmem64_phys_to_ptr(nq_u64 phys);
nq_u64 vmem64_ptr_to_phys(const void* ptr);

int vmem64_is_direct(nq_u64 virt);
int vmem64_is_user(nq_u64 virt);
int vmem64_is_identity(nq_u64 virt);

void vmem64_print_layout(void);

#endif