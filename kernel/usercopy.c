#include "usercopy.h"
#include "process.h"
#include "paging.h"

typedef unsigned char u8;
typedef unsigned int u32;

#define USER_TOP 0xC0000000u

static int range_wraps(u32 addr, u32 len) {
    if (len == 0) {
        return 0;
    }

    return addr + len < addr;
}

int user_ptr_readable(u32 user_addr, u32 len) {
    process_t* p = process_current();

    if (!p || !p->page_directory) {
        return 0;
    }

    if (len == 0) {
        return 1;
    }

    if (range_wraps(user_addr, len)) {
        return 0;
    }

    if (user_addr >= USER_TOP || user_addr + len > USER_TOP) {
        return 0;
    }

    for (u32 off = 0; off < len; off++) {
        u32 addr = user_addr + off;
        u32 phys = paging_get_physical_in_directory(p->page_directory, addr);

        if (!phys) {
            return 0;
        }
    }

    return 1;
}

int user_copy_from(void* kernel_dst, u32 user_src, u32 len) {
    process_t* p = process_current();
    u8* dst = (u8*)kernel_dst;

    if (!dst) {
        return 0;
    }

    if (!p || !p->page_directory) {
        return 0;
    }

    if (len == 0) {
        return 1;
    }

    if (range_wraps(user_src, len)) {
        return 0;
    }

    if (user_src >= USER_TOP || user_src + len > USER_TOP) {
        return 0;
    }

    for (u32 i = 0; i < len; i++) {
        u32 phys = paging_get_physical_in_directory(p->page_directory, user_src + i);

        if (!phys) {
            return 0;
        }

        dst[i] = *((u8*)phys);
    }

    return 1;
}

int user_copy_to(u32 user_dst, const void* kernel_src, u32 len) {
    process_t* p = process_current();
    const u8* src = (const u8*)kernel_src;

    if (!src) {
        return 0;
    }

    if (!p || !p->page_directory) {
        return 0;
    }

    if (len == 0) {
        return 1;
    }

    if (range_wraps(user_dst, len)) {
        return 0;
    }

    if (user_dst >= USER_TOP || user_dst + len > USER_TOP) {
        return 0;
    }

    for (u32 i = 0; i < len; i++) {
        u32 phys = paging_get_physical_in_directory(p->page_directory, user_dst + i);

        if (!phys) {
            return 0;
        }

        *((u8*)phys) = src[i];
    }

    return 1;
}

int user_copy_string(char* kernel_dst, u32 user_src, u32 max_len) {
    process_t* p = process_current();

    if (!kernel_dst || max_len == 0) {
        return 0;
    }

    if (!p || !p->page_directory) {
        kernel_dst[0] = 0;
        return 0;
    }

    if (user_src >= USER_TOP) {
        kernel_dst[0] = 0;
        return 0;
    }

    for (u32 i = 0; i < max_len - 1; i++) {
        u32 phys = paging_get_physical_in_directory(p->page_directory, user_src + i);

        if (!phys) {
            kernel_dst[i] = 0;
            return 0;
        }

        char c = *((char*)phys);
        kernel_dst[i] = c;

        if (c == 0) {
            return 1;
        }
    }

    kernel_dst[max_len - 1] = 0;
    return 1;
}