#ifndef NAQ_LIBC_STRING_H
#define NAQ_LIBC_STRING_H

#include <stddef.h>

/* strlen, memset and memcpy live in the app runtime (runtime/string.c). */
size_t strlen(const char* s);
void* memset(void* dst, int value, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);

int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* hay, const char* needle);

#endif
