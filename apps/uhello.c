#include "syscall.h"

void app_main(void) {
    app_write("Hello from a syscall-only ELF app.\n");
    app_write("PID: ");
    app_print_dec32(app_getpid());
    app_write("\nTicks: ");
    app_print_dec32(app_ticks());
    app_write("\n");

    app_exit(0);
}