#include "syscall.h"

void app_main(void) {
    app_write("Starting a 5 second timer...\n");
    app_sleep(5000);
    app_write("Timer done!");
}