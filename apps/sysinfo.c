#include "syscall.h"

void app_main(void) {
    app_write("==============================\n");
    app_write("        NAQ OS SYSINFO        \n");
    app_write("==============================\n\n");

    app_write("Current PID: ");
    app_print_dec32(app_getpid());
    app_write("\n");

    app_write("System ticks: ");
    app_print_dec32(app_ticks());
    app_write("\n");

    app_write("\nSubsystems:\n");
    app_write("  Bootloader        OK\n");
    app_write("  Protected Mode    OK\n");
    app_write("  IDT / IRQ         OK\n");
    app_write("  PIT Timer         OK\n");
    app_write("  FAT16             OK\n");
    app_write("  Paging            OK\n");
    app_write("  Heap              OK\n");
    app_write("  Processes         OK\n");
    app_write("  Scheduler         OK\n");
    app_write("  Terminals         OK\n");

    app_write("\nCommands to try from KER:\n");
    app_write("  ps\n");
    app_write("  spawn spin.naq\n");
    app_write("  fg <pid>\n");
    app_write("  Ctrl+A / Ctrl+C\n");

    app_write("\nSYSINFO complete.\n");
}