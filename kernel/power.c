#include "power.h"

typedef unsigned char  u8;
typedef unsigned short u16;

static void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outw(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

void kernel_reboot(void) {
    __asm__ volatile ("cli");

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

void kernel_shutdown(void) {
    __asm__ volatile ("cli");

    // QEMU/Bochs-friendly shutdown paths
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}