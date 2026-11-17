#ifndef USB64_H
#define USB64_H

#include "naq_boot_info.h"

/*
 * USB core: finds devices on the controllers' ports and behind hubs,
 * reads their descriptors and hands keyboards and mice to usbhid64.c.
 *
 * Everything runs in one kernel task ("usb") started by usb64_init().
 */

/*
 * Starts the USB task. Safe to call when the machine has no xHCI
 * controller; the task then simply ends.
 */
void usb64_init(void);

/*
 * Waits (briefly) for the devices that were plugged in at boot to be set
 * up, then prints what was found. Call once, before the shell starts.
 */
void usb64_boot_report(void);

/*
 * The "usb" shell command.
 */
void usb64_print_status(void);

/*
 * Message log, kept in memory and shown by the "usb" command.
 */
void usb64_log(const char* s);
void usb64_log_hex(nq_u64 value, nq_u32 digits);
void usb64_log_dec(nq_u64 value);

#endif
