#include "syscall.h"

void app_main(void) {
    app_clear();
    app_set_raw_input(1);

    app_write("RAW KEY TEST\n");
    app_write("Press keys. Press q to quit.\n\n");

    for (;;) {
        int c = app_read_key();

        if (c == 'q' || c == 'Q') {
            break;
        }

        app_write("Key: ");
        app_putchar((char)c);
        app_write("\n");
    }

    app_set_raw_input(0);
}