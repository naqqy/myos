#ifndef BOOTLOG64_H
#define BOOTLOG64_H

#include "naq_boot_info.h"

#ifndef NAQ_BOOT_VERBOSE
#define NAQ_BOOT_VERBOSE 0
#endif

int bootlog64_verbose(void);

void bootlog64_print(const char* s);
void bootlog64_print_hex(const char* label, nq_u64 value);
void bootlog64_print_dec(const char* label, nq_u64 value);

#endif