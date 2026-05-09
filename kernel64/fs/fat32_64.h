#ifndef FAT32_64_H
#define FAT32_64_H

#include "naq_boot_info.h"

int fat32_64_init(nq_u64 base_lba);
int fat32_64_mounted(void);

void fat32_64_print_summary(void);

int fat32_64_list_path(const char* path);
int fat32_64_read_file_path(
    const char* path,
    nq_u8* out_buf,
    nq_u64 max_bytes,
    nq_u64* out_size
);

#endif