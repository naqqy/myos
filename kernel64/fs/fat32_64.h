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

int fat32_64_stat_path(const char* path, nq_u64* out_size, int* out_is_dir);

typedef void (*fat32_64_enum_cb_t)(
    const char* name,
    nq_u64 size,
    int is_dir,
    void* ctx
);

/*
 * Calls cb for every entry directly inside the directory at `path`.
 */
int fat32_64_enum_path(const char* path, fat32_64_enum_cb_t cb, void* ctx);

int fat32_64_write_file_path(
    const char* path,
    const nq_u8* data,
    nq_u64 size
);

int fat32_64_mkdir_path(const char* path);
int fat32_64_rm_path(const char* path);
int fat32_64_rmdir_path(const char* path);

#endif