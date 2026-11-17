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

typedef void (*vfs64_enum_cb_t)(
    const char* name,
    nq_u64 size,
    int is_dir,
    void* ctx
);

/*
 * Calls cb for every entry directly inside the directory at `path`.
 * Returns 0 if the directory cannot be listed.
 */
int vfs64_enum_dir(const char* path, vfs64_enum_cb_t cb, void* ctx);

int vfs64_mount_root(void);

/*
 * A USB disk with a FAT32 filesystem appears at /usb. Called by the USB
 * mass-storage driver when the disk is plugged in or pulled out.
 * Mounting fails if FAT32 is already in use for the root filesystem.
 */
int vfs64_usb_mount(nq_u32 block_device_id, nq_u64 base_lba);
void vfs64_usb_unmount(void);
int vfs64_usb_mounted(void);

/*
 * 1 if the path is on a disk whose writes are currently locked (so the
 * caller can say "unlock it first" rather than "cannot write there").
 */
int vfs64_write_locked(const char* path);
int vfs64_mounted(void);

void vfs64_print_summary(void);

int vfs64_normalize_path(const char* input, char* output, nq_u64 output_size);

int vfs64_list_dir(const char* path);
/*
 * Reads a whole file into a kmalloc64() buffer sized to the file.
 * The caller owns *out_buf and must kfree64() it. Fails if the file is
 * missing, is a directory, or is larger than max_bytes.
 */
int vfs64_read_file_alloc(
    const char* path,
    nq_u64 max_bytes,
    void** out_buf,
    nq_u64* out_size
);

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