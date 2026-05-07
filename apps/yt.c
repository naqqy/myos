#include "syscall.h"

void app_main(void) {
    for (int i = 0; i < 5; i++) {
        app_write("YIELDTEST running...\n");
        app_yield();
    }

    app_write("YIELDTEST done.\n");
}