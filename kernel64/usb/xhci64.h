#ifndef XHCI64_H
#define XHCI64_H

#include "naq_boot_info.h"

/*
 * xHCI host controller driver.
 *
 * xHCI is the USB controller found in every PC from roughly 2012 on; it
 * drives low/full/high speed (USB 1/2) and SuperSpeed (USB 3) devices alike.
 *
 * The driver is polled: nothing here runs from an interrupt.
 *
 * Locking rule: every call must be made with task switching disabled
 * (process64_preempt_disable). The USB task (usb64.c) holds that guard
 * whenever it runs and lets go of it only while it sleeps inside one of
 * the waits below. Other tasks (a program reading a file from a USB disk)
 * take the guard themselves and never sleep: their waits spin instead.
 * So two tasks are never inside the driver at the same instant, and the
 * only interleaving happens at the USB task's sleeps.
 */

#define XHCI64_MAX_CONTROLLERS 4u
#define XHCI64_MAX_SLOTS       32u
#define XHCI64_MAX_PORTS       64u

/*
 * Port speed IDs as the controller reports them.
 */
#define XHCI64_SPEED_FULL  1u
#define XHCI64_SPEED_LOW   2u
#define XHCI64_SPEED_HIGH  3u
#define XHCI64_SPEED_SUPER 4u

typedef struct xhci64 xhci64_t;

/*
 * Called for every packet that arrives on an interrupt IN endpoint.
 * Runs inside event processing: it must not start transfers or sleep.
 */
typedef void (*xhci64_in_handler_t)(void* ctx, const nq_u8* data, nq_u32 length);

/*
 * Where a new device sits in the USB tree.
 */
typedef struct xhci64_device_info {
    nq_u8  root_port;   /* 1-based port on the controller itself */
    nq_u32 route;       /* route string: 4 bits of hub port per tier */
    nq_u8  speed;       /* XHCI64_SPEED_* */
    nq_u8  tt_slot;     /* high-speed hub translating for a slower device */
    nq_u8  tt_port;
} xhci64_device_info_t;

typedef struct xhci64_endpoint {
    nq_u8  address;     /* bEndpointAddress */
    nq_u8  attributes;  /* bmAttributes */
    nq_u16 max_packet;  /* wMaxPacketSize */
    nq_u8  interval;    /* bInterval */
} xhci64_endpoint_t;

/*
 * Finds every xHCI controller, takes it over from the firmware and starts
 * it. Returns how many are running.
 */
nq_u32 xhci64_init_all(void);

/*
 * Called once by the USB task before anything else: remembers which task
 * it is and takes the task-switch guard for it.
 */
void xhci64_enter_task(void);

/*
 * The USB task's own sleep: releases the guard while sleeping.
 */
void xhci64_task_sleep(nq_u32 ticks);

nq_u32    xhci64_count(void);
xhci64_t* xhci64_get(nq_u32 index);

/*
 * Handles everything the controller has reported since the last call.
 */
void xhci64_poll(xhci64_t* hc);

/*
 * Restarts endpoints that stopped after an error. Call after the port scan
 * so that an unplugged device is removed before anyone tries to revive it.
 */
void xhci64_recover(xhci64_t* hc);

/*
 * Sleeps while still handling events of all controllers.
 */
void xhci64_idle_ms(nq_u32 ms);

/*
 * Root hub ports, numbered from 1.
 */
nq_u32 xhci64_port_count(xhci64_t* hc);
int    xhci64_port_is_usb3(xhci64_t* hc, nq_u32 port);

/*
 * 1 if the controller reported a port change since the last call.
 */
int xhci64_take_port_event(xhci64_t* hc);

/*
 * Reads and acknowledges the port state. *out_changed is set if a connect
 * or disconnect happened since the last call.
 */
int xhci64_port_connected(xhci64_t* hc, nq_u32 port, int* out_changed);

/*
 * Resets the port so the device on it can be addressed.
 * Returns 1 and the device speed on success.
 */
int xhci64_port_reset(xhci64_t* hc, nq_u32 port, nq_u8* out_speed);

/*
 * Creates a device slot and gives the device an address.
 * Returns the slot ID (1..XHCI64_MAX_SLOTS) or 0.
 */
nq_u8 xhci64_device_open(xhci64_t* hc, const xhci64_device_info_t* info);
void  xhci64_device_close(xhci64_t* hc, nq_u8 slot);

int xhci64_set_ep0_max_packet(xhci64_t* hc, nq_u8 slot, nq_u32 max_packet);

/*
 * One control transfer on endpoint 0. data may be null when length is 0.
 * Returns the number of data bytes moved, or -1 on error.
 */
int xhci64_control(
    xhci64_t* hc,
    nq_u8 slot,
    nq_u8 request_type,
    nq_u8 request,
    nq_u16 value,
    nq_u16 index,
    void* data,
    nq_u16 length
);

/*
 * Tells the controller about the device's endpoints (count may be 0) and,
 * for a hub, how many ports it has.
 */
int xhci64_configure(
    xhci64_t* hc,
    nq_u8 slot,
    const xhci64_endpoint_t* endpoints,
    nq_u32 count,
    int is_hub,
    nq_u32 hub_ports,
    nq_u32 hub_ttt
);

/*
 * Starts receiving on an interrupt IN endpoint that xhci64_configure() set
 * up. The handler is called for every packet until the device is closed.
 */
int xhci64_interrupt_in_start(
    xhci64_t* hc,
    nq_u8 slot,
    nq_u8 endpoint_address,
    xhci64_in_handler_t handler,
    void* ctx
);

/*
 * Starts receiving continuously on a bulk IN endpoint (for devices that
 * send whenever they have something, such as a Bluetooth controller).
 * The handler gets each transfer, up to 4 KiB, as it arrives. The
 * endpoint can no longer be used with xhci64_bulk().
 */
int xhci64_bulk_in_listen(
    xhci64_t* hc,
    nq_u8 slot,
    nq_u8 endpoint_address,
    xhci64_in_handler_t handler,
    void* ctx
);

/*
 * One bulk transfer. `dma` must be memory from the physical frame
 * allocator (identity mapped), at most 64 KiB, and must not cross a
 * 64 KiB boundary.
 *
 * Returns 1 on success (with the byte count), 0 on error, -1 if the
 * device stalled the endpoint; in the last two cases the endpoint has
 * been reset on the controller side and the caller should send
 * CLEAR_FEATURE(ENDPOINT_HALT) to the device.
 */
int xhci64_bulk(
    xhci64_t* hc,
    nq_u8 slot,
    nq_u8 endpoint_address,
    void* dma,
    nq_u32 length,
    nq_u32* out_moved
);

void xhci64_print_status(void);

#endif
