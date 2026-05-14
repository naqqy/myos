#include "include/naqos.h"

#if SYS64_WRITE != 0
#error "HELLO_C is using wrong SYS64_WRITE"
#endif

#if SYS64_EXIT != 1
#error "HELLO_C is using wrong SYS64_EXIT"
#endif

#if SYS64_GET_TICKS != 2
#error "HELLO_C is using wrong SYS64_GET_TICKS"
#endif

static void print_u64(u64 value) {
    char buf[32];
    int i = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value > 0 && i < (int)(sizeof(buf))) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        putchar(buf[--i]);
    }
}

int main(void) {
    puts("Hello from a C ELF64 user app!\n");
    puts("Ticks: ");
    print_u64(get_ticks());
    puts("\n");

    return 0;
}