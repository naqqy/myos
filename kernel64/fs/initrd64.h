#ifndef INITRD64_H
#define INITRD64_H

#include "naq_boot_info.h"

int initrd64_init(nq_u64 base, nq_u64 size);
int initrd64_available(void);

void initrd64_print_summary(void);

int initrd64_list_path(const char* path);
int initrd64_file_size(const char* path, nq_u64* out_size);

typedef void (*initrd64_enum_cb_t)(
    const char* name,
    nq_u64 size,
    int is_dir,
    void* ctx
);

/*
 * Calls cb for every file directly inside the directory at `path`
 * (the INITRD is a flat list of full paths; subdirectories are not reported).
 */
int initrd64_enum_path(const char* path, initrd64_enum_cb_t cb, void* ctx);

int initrd64_read_file(
    const char* path,
    void* out_buf,
    nq_u64 max_bytes,
    nq_u64* out_size
);

#endif