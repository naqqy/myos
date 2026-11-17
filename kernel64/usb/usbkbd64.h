#ifndef USBKBD64_H
#define USBKBD64_H

#include "naq_boot_info.h"

void usbkbd64_init(void);
void usbkbd64_poll(void);
int  usbkbd64_available(void);

#endif
