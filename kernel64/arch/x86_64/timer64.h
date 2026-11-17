#ifndef TIMER64_H
#define TIMER64_H

#include "naq_boot_info.h"

int    timer64_init(nq_u32 frequency_hz);
void   timer64_on_irq(void);

nq_u64 timer64_get_ticks(void);
nq_u32 timer64_get_frequency(void);

/*
 * Date and time from the battery-backed clock (CMOS RTC). On a PC that
 * also runs Windows this is local time.
 */
typedef struct timer64_datetime {
    nq_u32 year;    /* e.g. 2026 */
    nq_u32 month;   /* 1..12 */
    nq_u32 day;     /* 1..31 */
    nq_u32 hour;    /* 0..23 */
    nq_u32 minute;
    nq_u32 second;
} timer64_datetime_t;

int timer64_read_clock(timer64_datetime_t* out);

void   timer64_sleep_ticks(nq_u64 ticks);
void   timer64_sleep_ms(nq_u64 ms);

#endif