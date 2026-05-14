#include "../include/naqos.h"

u64 time_now_ticks(void) {
    return get_ticks();
}

u64 time_ticks_per_second(void) {
    /*
     * Kernel PIT is currently initialized at 100 Hz.
     * Later this should come from a SYS_GET_TIMER_HZ syscall.
     */
    return NAQ_TIMER_HZ;
}

u64 time_ms_to_ticks(u64 ms) {
    u64 hz = time_ticks_per_second();

    /*
     * Round up so sleep_ms(1) does not become 0 ticks.
     */
    return ((ms * hz) + 999ull) / 1000ull;
}

u64 time_ticks_to_ms(u64 ticks) {
    u64 hz = time_ticks_per_second();

    if (hz == 0) {
        return 0;
    }

    return (ticks * 1000ull) / hz;
}

void sleep_ms(u64 ms) {
    u64 ticks = time_ms_to_ticks(ms);

    if (ticks == 0) {
        yield();
        return;
    }

    sleep_ticks(ticks);
}

void frame_timer_init(naq_frame_timer_t* timer, u64 target_fps) {
    if (!timer) {
        return;
    }

    if (target_fps == 0) {
        target_fps = 30;
    }

    timer->target_fps = target_fps;

    /*
     * Round up to at least one tick.
     * At 100 Hz:
     *   60 FPS -> about 2 ticks
     *   30 FPS -> about 4 ticks
     *   20 FPS -> 5 ticks
     */
    timer->frame_ticks =
        (time_ticks_per_second() + target_fps - 1ull) / target_fps;

    if (timer->frame_ticks == 0) {
        timer->frame_ticks = 1;
    }

    timer->last_tick = time_now_ticks();
    timer->frame_start = timer->last_tick;
    timer->frame_end = timer->last_tick;
    timer->frame_delta = 0;
    timer->frame_count = 0;
}

void frame_timer_begin(naq_frame_timer_t* timer) {
    if (!timer) {
        return;
    }

    timer->frame_start = time_now_ticks();

    if (timer->last_tick == 0) {
        timer->last_tick = timer->frame_start;
    }

    timer->frame_delta = timer->frame_start - timer->last_tick;
}

void frame_timer_end(naq_frame_timer_t* timer) {
    if (!timer) {
        return;
    }

    u64 now = time_now_ticks();
    u64 elapsed = now - timer->frame_start;

    while (elapsed < timer->frame_ticks) {
        sleep_ticks(timer->frame_ticks - elapsed);
        now = time_now_ticks();
        elapsed = now - timer->frame_start;
    }

    timer->frame_end = now;
    timer->last_tick = timer->frame_start;
    timer->frame_count++;
}

u64 frame_timer_delta_ticks(const naq_frame_timer_t* timer) {
    if (!timer) {
        return 0;
    }

    return timer->frame_delta;
}

u64 frame_timer_delta_ms(const naq_frame_timer_t* timer) {
    if (!timer) {
        return 0;
    }

    return time_ticks_to_ms(timer->frame_delta);
}