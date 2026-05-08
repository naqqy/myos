#ifndef IRQ64_H
#define IRQ64_H

#include "naq_boot_info.h"
#include "idt64.h"

void irq64_init(nq_u32 timer_hz);
void irq_handler64(irq_frame64_t* frame);

#endif