#ifndef SAFE_MODE64_H
#define SAFE_MODE64_H

#include "naq_boot_info.h"

#ifndef NAQ_SAFE_HW_DEFAULT
#define NAQ_SAFE_HW_DEFAULT 1
#endif

void safe_mode64_init(void);

int safe_mode64_enabled(void);
int safe_mode64_disk_writes_allowed(void);

void safe_mode64_unlock_disk_writes(void);
void safe_mode64_lock_disk_writes(void);

void safe_mode64_print_status(void);

#endif