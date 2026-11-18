#ifndef BT64_H
#define BT64_H

#include "naq_boot_info.h"
#include "../usb/xhci64.h"

/*
 * Bluetooth.
 *
 * A Bluetooth controller in a PC is a USB device, even when it is built
 * into the board (the wireless card's Bluetooth half is wired to an
 * internal USB port). The computer talks to it in a language called HCI:
 *
 *   commands   sent to the device's control endpoint
 *   events     the controller's answers, on an interrupt endpoint
 *   data       packets to and from other Bluetooth devices, on two bulk
 *              endpoints
 *
 * This driver finds the controller, sets it up, scans for and pairs with
 * a headset, and plays the system's sound through it (A2DP with the SBC
 * codec; see bta2dp64.c and sbc64.c).
 *
 * Not done: loading the controller's firmware. Most controllers start
 * with no program in them. Until there is a loader, the firmware has to
 * be left in the chip by another system: start Windows, then restart
 * (not shut down) into NAQ OS.
 */

typedef struct bt64_interface {
    nq_u8  number;
    nq_u8  ep_events;       /* interrupt IN */
    nq_u8  ep_data_in;      /* bulk IN */
    nq_u8  ep_data_out;     /* bulk OUT */
    nq_u16 events_packet;
    nq_u16 data_in_packet;
    nq_u16 data_out_packet;
    nq_u8  events_interval;
} bt64_interface_t;

/*
 * Called by the USB task when a Bluetooth controller has been configured
 * and when it goes away.
 */
int bt64_attach(xhci64_t* hc, nq_u8 slot, const bt64_interface_t* intf,
                nq_u16 vendor, nq_u16 product);
void bt64_detach(xhci64_t* hc, nq_u8 slot);

int bt64_present(void);

/* Starts the Bluetooth task. */
void bt64_init(void);

/* The "bt" shell command: bt, bt pair, bt scan, bt connect [n],
   bt disconnect, bt forget, bt log. */
void bt64_shell(const char* args);

#endif
