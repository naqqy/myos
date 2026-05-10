#ifndef VFS64_H
#define VFS64_H

#include "naq_boot_info.h"

#define VFS64_MAX_PATH 256

int vfs64_mount_root(void);
int vfs64_mounted(void);

void vfs64_print_summary(void);

int vfs64_normalize_path(const char* input, char* output, nq_u64 output_size);

int vfs64_list_dir(const char* path);
int vfs64_read_file(
    const char* path,
    void* out_buf,
    nq_u64 max_bytes,
    nq_u64* out_size
);

int vfs64_write_file(
    const char* path,
    const void* data,
    nq_u64 size
);

int vfs64_mkdir(const char* path);
int vfs64_rm(const char* path);
int vfs64_rmdir(const char* path);

#endif