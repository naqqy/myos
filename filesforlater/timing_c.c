#include "include/naqos.h"

static void print_u64(u64 value) {
    char buf[32];
    int i = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        putchar(buf[--i]);
    }
}

int main(void) {
    naq_frame_timer_t frame;

    puts("Timing syscall test\n");

    puts("pid=");
    print_u64(getpid());
    puts("\n");

    puts("start ticks=");
    print_u64(get_ticks());
    puts("\n");

    puts("sleep_ms(20000)...\n");
    sleep_ms(20000);

    puts("after sleep ticks=");
    print_u64(get_ticks());
    puts("\n");

    frame_timer_init(&frame, 20);

    for (u64 i = 0; i < 5; i++) {
        frame_timer_begin(&frame);

        puts("frame ");
        print_u64(i);
        puts(" delta_ms=");
        print_u64(frame_timer_delta_ms(&frame));
        puts(" ticks=");
        print_u64(get_ticks());
        puts("\n");

        frame_timer_end(&frame);
    }

    puts("timing test done\n");
    return 0;
}