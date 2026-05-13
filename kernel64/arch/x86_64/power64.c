#include "power64.h"

#include "io64.h"
#include "../../k64_console.h"

static void power64_io_wait(void) {
    /*
     * Traditional tiny delay.
     */
    outb(0x80, 0);
}

static void power64_halt_forever(void) {
    __asm__ volatile ("cli");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static int power64_wait_kbc_input_clear(void) {
    for (unsigned int i = 0; i < 1000000u; i++) {
        /*
         * Port 0x64 status bit 1 = input buffer full.
         * Wait until it clears.
         */
        if ((inb(0x64) & 0x02u) == 0) {
            return 1;
        }

        power64_io_wait();
    }

    return 0;
}

static void power64_reboot_cf9(void) {
    /*
     * PCI reset control register.
     *
     * This works on a lot of real PCs/chipsets and many emulators.
     *
     * Common sequence:
     *   0x02 = system reset request
     *   0x06 = hard reset/full reset
     */
    outb(0xCF9, 0x02);
    power64_io_wait();
    outb(0xCF9, 0x06);

    power64_io_wait();
}

static void power64_reboot_keyboard_controller(void) {
    /*
     * Legacy 8042 keyboard controller CPU reset.
     * Not always available on modern systems, but still useful fallback.
     */
    if (power64_wait_kbc_input_clear()) {
        outb(0x64, 0xFE);
    }

    power64_io_wait();
}

static void power64_triple_fault(void) {
    /*
     * Last-resort reset:
     * load an empty IDT and trigger an interrupt.
     * The CPU should triple fault and reset.
     */
    struct {
        unsigned short limit;
        unsigned long long base;
    } __attribute__((packed)) idtr = { 0, 0 };

    __asm__ volatile (
        "cli\n"
        "lidt %0\n"
        "int $0x03\n"
        :
        : "m"(idtr)
        : "memory"
    );
}

void power64_reboot(void) {
    k64_print("Rebooting...\n");

    __asm__ volatile ("cli");

    /*
     * Best current real-hardware attempt.
     */
    power64_reboot_cf9();

    /*
     * Fallback for systems with legacy 8042 support.
     */
    power64_reboot_keyboard_controller();

    /*
     * Final fallback.
     */
    power64_triple_fault();

    power64_halt_forever();
}

static void power64_shutdown_qemu(void) {
    /*
     * QEMU isa-debug-exit / ACPI-ish debug ports.
     * These are emulator-specific and normally do nothing on real hardware.
     */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
}

static void power64_shutdown_bochs_old(void) {
    /*
     * Bochs/QEMU older shutdown magic.
     */
    outw(0xB004, 0x2000);
}

void power64_shutdown(void) {
    k64_print("Shutdown requested.\n");

    /*
     * Emulator shutdown attempts.
     */
    power64_shutdown_qemu();
    power64_shutdown_bochs_old();

    /*
     * Real hardware shutdown requires ACPI S5 support.
     * Until ACPI is implemented, do not pretend this powered off.
     */
    k64_print("ACPI shutdown is not implemented yet.\n");
    k64_print("System halted. Hold power button to turn off.\n");

    power64_halt_forever();
}