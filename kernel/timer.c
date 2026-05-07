#include "timer.h"

typedef unsigned char  u8;
typedef unsigned short u16;

static volatile u32 timer_ticks = 0;
static u32 timer_frequency_hz = 100;

static void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

int timer_init(u32 frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 100;
    }

    timer_frequency_hz = frequency_hz;

    u32 divisor = 1193182U / frequency_hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    outb(0x43, 0x36);
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)((divisor >> 8) & 0xFF));

    return 1; // Assume success
}

void timer_on_irq(void) {
    timer_ticks++;
}

u32 timer_get_ticks(void) {
    return timer_ticks;
}

u32 timer_get_frequency(void) {
    return timer_frequency_hz;
}

void timer_sleep_ticks(u32 ticks) {
    u32 start = timer_ticks;
    while ((u32)(timer_ticks - start) < ticks) {
        __asm__ volatile ("hlt");
    }
}

void timer_sleep_ms(u32 ms) {
    u32 ticks = (ms * timer_frequency_hz + 999U) / 1000U;
    if (ticks == 0 && ms != 0) {
        ticks = 1;
    }
    timer_sleep_ticks(ticks);
}