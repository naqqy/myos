#include "syscall.h"

void app_main(void) {
    app_write("SPIN: starting infinite loop. Press Ctrl+C to kill me.\n");

    volatile unsigned int x = 0;

    for (;;) {
        x++;
    }
}