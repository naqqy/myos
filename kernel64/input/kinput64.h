#ifndef KINPUT64_H
#define KINPUT64_H

#include "naq_boot_info.h"

/*
 * Keys without an ASCII code are delivered as these values (above 127).
 * The same numbers are exposed to apps as NAQ_KEY_* in naqos.h.
 */
#define KINPUT64_KEY_UP        0x80
#define KINPUT64_KEY_DOWN      0x81
#define KINPUT64_KEY_LEFT      0x82
#define KINPUT64_KEY_RIGHT     0x83
#define KINPUT64_KEY_PAGE_UP   0x84
#define KINPUT64_KEY_PAGE_DOWN 0x85
#define KINPUT64_KEY_HOME      0x86
#define KINPUT64_KEY_END       0x87
#define KINPUT64_KEY_DELETE    0x88

/*
 * Alt+Tab, for the desktop's window switcher. Not passed on to apps.
 */
#define KINPUT64_KEY_ALT_TAB   0x89

void kinput64_init(void);

void kinput64_push_char(char c);
int  kinput64_poll_char(void);
char kinput64_read_char(void);

#endif