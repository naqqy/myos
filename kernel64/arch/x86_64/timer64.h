#ifndef TIMER64_H
#define TIMER64_H

#include "naq_boot_info.h"

int    timer64_init(nq_u32 frequency_hz);
void   timer64_on_irq(void);

nq_u64 timer64_get_ticks(void);
nq_u32 timer64_get_frequency(void);

void   timer64_sleep_ticks(nq_u64 ticks);
void   timer64_sleep_ms(nq_u64 ms);

#endif