#ifndef USBMSC64_H
#define USBMSC64_H

#include "naq_boot_info.h"
#include "xhci64.h"

/*
 * USB mass storage: memory sticks, card readers and external drives that
 * speak the usual "bulk-only" protocol with SCSI commands.
 *
 * One disk is supported at a time (the first one found). It is registered
 * with block64 as a block device and, if it holds a FAT32 filesystem,
 * mounted at /usb.
 */

typedef struct usbmsc64_interface {
    nq_u8  number;
    nq_u8  ep_in;       /* bulk IN endpoint address */
    nq_u8  ep_out;      /* bulk OUT endpoint address */
    nq_u16 in_packet;
    nq_u16 out_packet;
} usbmsc64_interface_t;

/*
 * Sets the disk up. Called by the USB task once the device is configured.
 * Returns 1 if the disk is usable.
 */
int usbmsc64_attach(xhci64_t* hc, nq_u8 slot, const usbmsc64_interface_t* intf);

void usbmsc64_detach(xhci64_t* hc, nq_u8 slot);

/*
 * Sector I/O (512-byte sectors). Callable from any task.
 */
int usbmsc64_read(nq_u64 lba, void* buffer, nq_u32 sector_count);
int usbmsc64_write(nq_u64 lba, const void* buffer, nq_u32 sector_count);

void usbmsc64_print_status(void);

#endif
