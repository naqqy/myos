#ifndef BLOCK64_H
#define BLOCK64_H

#include "naq_boot_info.h"

#define BLOCK64_SECTOR_SIZE 512u

int block64_init(void);
int block64_available(void);

int block64_read(nq_u64 lba, void* buffer, nq_u32 sector_count);
int block64_write(nq_u64 lba, const void* buffer, nq_u32 sector_count);

nq_u64 block64_sector_count(void);

void block64_print_summary(void);

#endif