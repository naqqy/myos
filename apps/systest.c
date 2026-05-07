#include "syscall.h"

void app_main(void) {
    app_write("Hello from ring 3!\n");
    app_write("SYS_WRITE from user mode works.\n");

    app_exit(0);
}