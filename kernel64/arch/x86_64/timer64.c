#include "timer64.h"
#include "io64.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182u

static volatile nq_u64 g_timer_ticks = 0;
static nq_u32 g_timer_frequency_hz = 100;

int timer64_init(nq_u32 frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 1000;
    }

    g_timer_ticks = 0;
    g_timer_frequency_hz = frequency_hz;

    nq_u32 divisor = PIT_BASE_HZ / frequency_hz;

    if (divisor == 0) {
        divisor = 1;
    }

    if (divisor > 65535u) {
        divisor = 65535u;
    }

    /*
     * PIT mode 3, lobyte/hibyte, channel 0.
     */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (nq_u8)(divisor & 0xFFu));
    outb(PIT_CHANNEL0, (nq_u8)((divisor >> 8) & 0xFFu));

    return 1;
}

void timer64_on_irq(void) {
    g_timer_ticks++;
}

nq_u64 timer64_get_ticks(void) {
    return g_timer_ticks;
}

nq_u32 timer64_get_frequency(void) {
    return g_timer_frequency_hz;
}

void timer64_sleep_ticks(nq_u64 ticks) {
    nq_u64 start = timer64_get_ticks();

    while ((timer64_get_ticks() - start) < ticks) {
        /*
         * This is intended for normal kernel code after interrupts are enabled.
         */
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
}

void timer64_sleep_ms(nq_u64 ms) {
    nq_u64 ticks;

    if (ms == 0) {
        return;
    }

    ticks = (ms * (nq_u64)g_timer_frequency_hz + 999ull) / 1000ull;

    if (ticks == 0) {
        ticks = 1;
    }

    timer64_sleep_ticks(ticks);
}