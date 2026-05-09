#ifndef IO64_H
#define IO64_H

#include "naq_boot_info.h"

static inline void outb(nq_u16 port, nq_u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(nq_u16 port, nq_u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline nq_u8 inb(nq_u16 port) {
    nq_u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline nq_u16 inw(nq_u16 port) {
    nq_u16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif