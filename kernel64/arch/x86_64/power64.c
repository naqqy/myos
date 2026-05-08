#include "power64.h"
#include "io64.h"

void power64_reboot(void) {
    __asm__ volatile ("cli");

    /*
     * Ask the PS/2 controller to pulse CPU reset.
     */
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) {
            break;
        }

        io_wait();
    }

    outb(0x64, 0xFE);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void power64_shutdown(void) {
    __asm__ volatile ("cli");

    /*
     * Emulator-friendly ACPI/Bochs/QEMU shutdown ports.
     */
    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}