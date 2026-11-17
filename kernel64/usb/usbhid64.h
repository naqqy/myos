#ifndef USBHID64_H
#define USBHID64_H

#include "naq_boot_info.h"
#include "xhci64.h"

/*
 * USB HID driver: keyboards and pointing devices (mice, and the absolute
 * "tablet" pointer that virtual machines offer).
 *
 * Keys are delivered through kinput64 and pointer movement through mouse64,
 * the same queues the PS/2 drivers feed, so the rest of the system does not
 * care which kind of device is attached.
 */

#define USBHID64_NONE     0
#define USBHID64_KEYBOARD 1
#define USBHID64_POINTER  2

/*
 * One HID interface of a device, as found in its configuration descriptor.
 */
typedef struct usbhid64_interface {
    nq_u8  number;
    nq_u8  subclass;        /* 1 = supports the simple "boot" protocol */
    nq_u8  protocol;        /* with subclass 1: 1 = keyboard, 2 = mouse */
    nq_u16 report_length;   /* size of the HID report descriptor */

    nq_u8  ep_address;      /* interrupt IN endpoint */
    nq_u16 ep_max_packet;
    nq_u8  ep_interval;
} usbhid64_interface_t;

/*
 * Starts the driver for one interface. Returns USBHID64_KEYBOARD,
 * USBHID64_POINTER, or USBHID64_NONE if it is something else (a gamepad,
 * a media-key interface, ...).
 */
int usbhid64_attach(xhci64_t* hc, nq_u8 slot, const usbhid64_interface_t* intf);

/*
 * Forgets every interface of a device that has been unplugged.
 */
void usbhid64_detach(xhci64_t* hc, nq_u8 slot);

/*
 * Key repeat and keyboard LEDs. Called from the USB task loop.
 */
void usbhid64_tick(void);

void usbhid64_print_status(void);

#endif
