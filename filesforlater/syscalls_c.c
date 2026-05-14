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
    puts("C syscall test\n");

    puts("pid=");
    print_u64(getpid());
    puts("\n");

    puts("ticks=");
    print_u64(get_ticks());
    puts("\n");

    puts("yielding...\n");
    yield();

    puts("sleeping 10 ticks...\n");
    sleep_ticks(10);

    puts("ticks=");
    print_u64(get_ticks());
    puts("\n");

    puts("done\n");

    return 0;
}