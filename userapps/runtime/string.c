#include "../include/naqos.h"

size_t strlen(const char* s) {
    size_t n = 0;

    if (!s) {
        return 0;
    }

    while (s[n]) {
        n++;
    }

    return n;
}

void* memset(void* dst, int value, size_t n) {
    u8* p = (u8*)dst;

    for (size_t i = 0; i < n; i++) {
        p[i] = (u8)value;
    }

    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }

    return dst;
}