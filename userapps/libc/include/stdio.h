/*
 * NAQ OS small C library: just enough of <stdio.h> for ported code such as
 * the QuickJS engine. There are no real files; stdout and stderr both go
 * to the console.
 */
#ifndef NAQ_LIBC_STDIO_H
#define NAQ_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct naq_file FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#define EOF (-1)

int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE* f, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE* f, const char* fmt, va_list ap);
int vprintf(const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int sprintf(char* buf, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

int fputc(int c, FILE* f);
int fputs(const char* s, FILE* f);
size_t fwrite(const void* data, size_t size, size_t count, FILE* f);
int fflush(FILE* f);

#define putc(c, f) fputc((c), (f))

/* These two come from the app runtime, which declares them without a result. */
void putchar(int c);
void puts(const char* s);

#endif
