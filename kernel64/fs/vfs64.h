#ifndef VFS64_H
#define VFS64_H

#include "naq_boot_info.h"

#define VFS64_MAX_PATH 256

typedef struct vfs64_stat {
    nq_u64 size;
    nq_u32 type;
} vfs64_stat_t;

#define VFS64_TYPE_UNKNOWN 0
#define VFS64_TYPE_FILE    1
#define VFS64_TYPE_DIR     2

int vfs64_stat(const char* path, vfs64_stat_t* out);

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

typedef enum vfs64_backend {
    VFS64_BACKEND_NONE = 0,
    VFS64_BACKEND_FAT32,
    VFS64_BACKEND_INITRD
} vfs64_backend_t;

vfs64_backend_t vfs64_root_backend(void);
const char* vfs64_backend_name(vfs64_backend_t backend);

int vfs64_safe_mode_root_policy(void);

#endif