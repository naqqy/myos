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

static nq_u8 cmos_read(nq_u8 reg) {
    outb(0x70, reg);
    io_wait();

    return inb(0x71);
}

static nq_u32 from_bcd(nq_u8 value) {
    return (nq_u32)(value & 0x0Fu) + (nq_u32)(value >> 4) * 10u;
}

int timer64_read_clock(timer64_datetime_t* out) {
    if (!out) {
        return 0;
    }

    /*
     * Wait out an update in progress (bit 7 of status A), so the fields
     * all belong to the same second.
     */
    for (nq_u32 i = 0; i < 100000u; i++) {
        if (!(cmos_read(0x0A) & 0x80u)) {
            break;
        }
    }

    nq_u8 second = cmos_read(0x00);
    nq_u8 minute = cmos_read(0x02);
    nq_u8 hour = cmos_read(0x04);
    nq_u8 day = cmos_read(0x07);
    nq_u8 month = cmos_read(0x08);
    nq_u8 year = cmos_read(0x09);
    nq_u8 status_b = cmos_read(0x0B);

    int pm = (hour & 0x80u) != 0;

    hour &= 0x7Fu;

    /*
     * Status B bit 2: values are binary rather than BCD.
     * Status B bit 1: 24-hour rather than 12-hour clock.
     */
    if (status_b & 0x04u) {
        out->second = second;
        out->minute = minute;
        out->hour = hour;
        out->day = day;
        out->month = month;
        out->year = 2000u + year;
    } else {
        out->second = from_bcd(second);
        out->minute = from_bcd(minute);
        out->hour = from_bcd(hour);
        out->day = from_bcd(day);
        out->month = from_bcd(month);
        out->year = 2000u + from_bcd(year);
    }

    if (!(status_b & 0x02u)) {
        out->hour %= 12u;

        if (pm) {
            out->hour += 12u;
        }
    }

    if (out->month < 1u || out->month > 12u || out->day < 1u || out->day > 31u ||
        out->hour > 23u || out->minute > 59u || out->second > 59u) {
        return 0;
    }

    return 1;
}
