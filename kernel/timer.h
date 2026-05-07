#ifndef TIMER_H
#define TIMER_H

typedef unsigned int u32;

int timer_init(u32 frequency_hz);
void timer_on_irq(void);
u32 timer_get_ticks(void);
u32 timer_get_frequency(void);
void timer_sleep_ticks(u32 ticks);
void timer_sleep_ms(u32 ms);

#endif