#include "usercopy64.h"
#include "paging64.h"

int usercopy64_check_read(const void* user_ptr, nq_u64 size) {
    return paging64_user_range_accessible((nq_u64)user_ptr, size);
}

int usercopy64_check_write(void* user_ptr, nq_u64 size) {
    /*
     * For now, readable user-accessible also means writable enough
     * for our POC because paging64_user_range_accessible only checks
     * USER/PRESENT, not RW.
     *
     * Later, add paging64_user_range_writable().
     */
    return paging64_user_range_accessible((nq_u64)user_ptr, size);
}

int usercopy64_copy_from_user(void* kernel_dst, const void* user_src, nq_u64 size) {
    nq_u8* dst = (nq_u8*)kernel_dst;
    const nq_u8* src = (const nq_u8*)user_src;

    if (size == 0) {
        return 1;
    }

    if (!dst || !src) {
        return 0;
    }

    if (!usercopy64_check_read(user_src, size)) {
        return 0;
    }

    for (nq_u64 i = 0; i < size; i++) {
        dst[i] = src[i];
    }

    return 1;
}

int usercopy64_copy_to_user(void* user_dst, const void* kernel_src, nq_u64 size) {
    nq_u8* dst = (nq_u8*)user_dst;
    const nq_u8* src = (const nq_u8*)kernel_src;

    if (size == 0) {
        return 1;
    }

    if (!dst || !src) {
        return 0;
    }

    if (!usercopy64_check_write(user_dst, size)) {
        return 0;
    }

    for (nq_u64 i = 0; i < size; i++) {
        dst[i] = src[i];
    }

    return 1;
}

nq_u64 usercopy64_strnlen_user(const char* user_str, nq_u64 max_len) {
    if (!user_str || max_len == 0) {
        return 0;
    }

    /*
     * Check one byte at a time so a string can stop before an invalid page.
     */
    for (nq_u64 i = 0; i < max_len; i++) {
        if (!usercopy64_check_read(user_str + i, 1)) {
            return i;
        }

        if (user_str[i] == 0) {
            return i;
        }
    }

    return max_len;
}

int usercopy64_copy_string_from_user(
    char* kernel_dst,
    const char* user_src,
    nq_u64 dst_size
) {
    if (!kernel_dst || !user_src || dst_size == 0) {
        return 0;
    }

    nq_u64 len = usercopy64_strnlen_user(user_src, dst_size - 1u);

    if (!usercopy64_copy_from_user(kernel_dst, user_src, len)) {
        kernel_dst[0] = 0;
        return 0;
    }

    kernel_dst[len] = 0;
    return 1;
}