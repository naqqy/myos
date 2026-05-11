#ifndef USERCOPY64_H
#define USERCOPY64_H

#include "naq_boot_info.h"

int usercopy64_check_read(const void* user_ptr, nq_u64 size);
int usercopy64_check_write(void* user_ptr, nq_u64 size);

int usercopy64_copy_from_user(void* kernel_dst, const void* user_src, nq_u64 size);
int usercopy64_copy_to_user(void* user_dst, const void* kernel_src, nq_u64 size);

nq_u64 usercopy64_strnlen_user(const char* user_str, nq_u64 max_len);
int usercopy64_copy_string_from_user(
    char* kernel_dst,
    const char* user_src,
    nq_u64 dst_size
);

#endif