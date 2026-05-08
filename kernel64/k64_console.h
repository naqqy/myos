#ifndef K64_CONSOLE_H
#define K64_CONSOLE_H

#include "naq_boot_info.h"

void k64_console_init(naq_boot_info_t* boot);
void k64_clear_screen(void);

void k64_print(const char* s);
void k64_putchar(char c);
void k64_backspace(void);

void k64_print_hex64(nq_u64 value);
void k64_print_dec64(nq_u64 value);

#endif