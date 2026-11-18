#ifndef NAQ_LIBC_STDLIB_H
#define NAQ_LIBC_STDLIB_H

#include <stddef.h>

/*
 * The heap is a block of memory the app hands over once at start-up
 * (usually a large static array), so each app decides how much it needs.
 * malloc() returns NULL until this has been called.
 */
void naq_heap_init(void* base, size_t size);
size_t naq_heap_used(void);

void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
size_t malloc_usable_size(void* ptr);

void abort(void) __attribute__((noreturn));
void exit(int code);

int abs(int v);
int atoi(const char* s);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
double strtod(const char* s, char** end);

void qsort(void* base, size_t count, size_t size,
           int (*cmp)(const void*, const void*));

#define alloca(n) __builtin_alloca(n)

#endif
