#ifndef KINPUT64_H
#define KINPUT64_H

#include "naq_boot_info.h"

void kinput64_init(void);

void kinput64_push_char(char c);
int  kinput64_poll_char(void);
char kinput64_read_char(void);

#endif