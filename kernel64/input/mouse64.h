#ifndef MOUSE64_H
#define MOUSE64_H

#include "naq_boot_info.h"

#define MOUSE64_BUTTON_LEFT   0x01u
#define MOUSE64_BUTTON_RIGHT  0x02u
#define MOUSE64_BUTTON_MIDDLE 0x04u

/*
 * One pointer update.
 *
 * Relative (a real PS/2 mouse): dx is positive to the right, dy is positive
 * DOWN (screen direction), absolute is 0.
 *
 * Absolute (the VMware-style "vmmouse" that QEMU, VMware and VirtualBox
 * provide): absolute is 1 and abs_x / abs_y give the pointer position as
 * 0..65535 across the screen; dx and dy are 0.
 */
typedef struct mouse64_event {
    int dx;
    int dy;
    nq_u8 buttons;

    int absolute;
    nq_u32 abs_x;
    nq_u32 abs_y;
} mouse64_event_t;

/*
 * Enables the PS/2 auxiliary (mouse) port and data reporting.
 * Must be called with interrupts disabled. Returns 1 if a mouse answered.
 */
int mouse64_init(void);

int mouse64_available(void);

/*
 * 1 if the pointer reports absolute positions (running in a VM).
 */
int mouse64_is_absolute(void);

/*
 * Feeds one byte read from the PS/2 controller's auxiliary device.
 * Called from interrupt context.
 */
void mouse64_handle_byte(nq_u8 data);

/*
 * Adds a pointer update from another driver (USB). Safe to call from a
 * task; also makes the pointer visible if there was no PS/2 mouse.
 */
void mouse64_push_event(const mouse64_event_t* ev);

/*
 * Takes the next packet from the queue. Returns 0 if there is none.
 */
int mouse64_poll(mouse64_event_t* out);

#endif
