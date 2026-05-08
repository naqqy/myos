#ifndef MEMMAP64_H
#define MEMMAP64_H

#include "naq_boot_info.h"

#define K64_MEMMAP_MAX_REGIONS 128

#define K64_MEM_REGION_RESERVED 0
#define K64_MEM_REGION_USABLE   1
#define K64_MEM_REGION_ACPI     2
#define K64_MEM_REGION_MMIO     3
#define K64_MEM_REGION_LOADER   4

typedef struct k64_mem_region {
    nq_u64 base;
    nq_u64 length;
    nq_u32 type;
    nq_u32 uefi_type;
    nq_u64 attributes;
} k64_mem_region_t;

void memmap64_init(const naq_boot_info_t* boot);

nq_u64 memmap64_region_count(void);
const k64_mem_region_t* memmap64_region(nq_u64 index);

nq_u64 memmap64_total_usable_bytes(void);
nq_u64 memmap64_highest_usable_address(void);

void memmap64_print_summary(void);
void memmap64_print_regions(void);

#endif