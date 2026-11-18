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
    void* p = dst;

    /* "rep stosb": store AL into RCX bytes at RDI */
    __asm__ volatile ("rep stosb"
                      : "+D"(p), "+c"(n)
                      : "a"(value)
                      : "memory");

    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    void* d = dst;

    /* "rep movsb": copy RCX bytes from RSI to RDI */
    __asm__ volatile ("rep movsb"
                      : "+D"(d), "+S"(src), "+c"(n)
                      :
                      : "memory");

    return dst;
}