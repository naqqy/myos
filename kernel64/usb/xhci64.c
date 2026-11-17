#include "xhci64.h"
#include "usb64.h"

#include "../hw/pci64.h"
#include "../k64_console.h"
#include "../mm/paging64.h"
#include "../mm/pmm64.h"
#include "../mm/vmem64.h"
#include "../arch/x86_64/timer64.h"
#include "../proc/process64.h"

/*
 * xHCI host controller driver. See xhci64.h for the rules of use.
 *
 * How xHCI works, briefly: the driver and the controller talk through
 * "rings" of 16-byte records (TRBs) in ordinary memory.
 *
 *   command ring   driver -> controller   "enable a slot", "address device"
 *   transfer rings driver -> controller   one per endpoint: "send/receive"
 *   event ring     controller -> driver   "that command/transfer finished"
 *
 * The driver writes TRBs and rings a "doorbell" register; the controller
 * answers on the event ring. Each device gets a "slot" with a context
 * (a block of memory describing the device and its endpoints) that the
 * controller reads and updates.
 *
 * All memory handed to the controller comes from the physical frame
 * allocator, which is identity mapped and below 4 GiB, so a pointer is
 * also the physical address the controller needs.
 */

#define RING_TRBS 256u

#define MMIO_MAP_SIZE 0x100000ull

/*
 * Capability registers.
 */
#define CAP_CAPLENGTH  0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18

/*
 * Operational registers.
 */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC   0x400

#define USBCMD_RUN   0x00000001u
#define USBCMD_RESET 0x00000002u

#define USBSTS_HALTED    0x00000001u
#define USBSTS_NOT_READY 0x00000800u

/*
 * Interrupter 0 registers, relative to the runtime base.
 */
#define IR0_IMAN   0x20
#define IR0_ERSTSZ 0x28
#define IR0_ERSTBA 0x30
#define IR0_ERDP   0x38

#define PORTSC_CONNECTED 0x00000001u
#define PORTSC_ENABLED   0x00000002u
#define PORTSC_RESET     0x00000010u
#define PORTSC_POWER     0x00000200u
#define PORTSC_CSC       0x00020000u
#define PORTSC_PRC       0x00200000u

/*
 * Bits that are safe to write back unchanged. Everything else in PORTSC is
 * either "write 1 to clear" or "write 1 to trigger", so a plain
 * read-modify-write would disable the port or wipe pending changes.
 */
#define PORTSC_PRESERVE  0x0E00C200u
#define PORTSC_CHANGES   0x00FE0000u

#define TRB_NORMAL         1u
#define TRB_SETUP          2u
#define TRB_DATA           3u
#define TRB_STATUS         4u
#define TRB_LINK           6u
#define TRB_ENABLE_SLOT    9u
#define TRB_DISABLE_SLOT   10u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_CONFIGURE_EP   12u
#define TRB_EVALUATE_CTX   13u
#define TRB_RESET_EP       14u
#define TRB_STOP_EP        15u
#define TRB_SET_DEQUEUE    16u

#define TRB_EVENT_TRANSFER 32u
#define TRB_EVENT_COMMAND  33u
#define TRB_EVENT_PORT     34u

#define TRB_TYPE(t)   ((nq_u32)(t) << 10)
#define TRB_CYCLE     0x00000001u
#define TRB_TOGGLE    0x00000002u
#define TRB_ISP       0x00000004u
#define TRB_IOC       0x00000020u
#define TRB_IDT       0x00000040u
#define TRB_DIR_IN    0x00010000u

#define CODE_SUCCESS      1u
#define CODE_SHORT_PACKET 13u
#define CODE_TIMEOUT      0xFFu

#define EP_STATE_RUNNING 1u
#define EP_STATE_HALTED  2u

#define EP_TYPE_BULK_OUT     2u
#define EP_TYPE_CONTROL      4u
#define EP_TYPE_BULK_IN      6u
#define EP_TYPE_INTERRUPT_IN 7u

#define CODE_STALL        6u

#define COMMAND_TIMEOUT_MS 2000u
#define CONTROL_TIMEOUT_MS 2000u
#define BULK_TIMEOUT_MS    5000u

/*
 * Upper bound for a spinning wait, in case timer ticks are not advancing
 * (a caller with interrupts off).
 */
#define SPIN_LIMIT 40000000u

/*
 * An endpoint that keeps failing is switched off after this many errors
 * in a row.
 */
#define EP_MAX_ERRORS 8u

typedef struct trb {
    nq_u32 p_lo;
    nq_u32 p_hi;
    nq_u32 status;
    nq_u32 control;
} trb_t;

typedef struct ring {
    trb_t* trbs;
    nq_u32 enqueue;
    nq_u32 cycle;
} ring_t;

typedef struct xhci_ep {
    int active;
    int halted;
    nq_u32 errors;

    ring_t ring;

    nq_u8* buffer;
    nq_u32 buffer_len;

    xhci64_in_handler_t handler;
    void* handler_ctx;

    /*
     * Bulk endpoints: the one transfer in flight.
     */
    int is_bulk;
    nq_u64 wait_trb;
    volatile int xfer_done;
    nq_u32 xfer_code;
    nq_u32 xfer_residual;
} xhci_ep_t;

/*
 * A command waiting for its completion event. Lives on the stack of the
 * task that issued it.
 */
typedef struct cmd_wait {
    nq_u64 trb;
    volatile int done;
    nq_u32 code;
    nq_u8 slot;
} cmd_wait_t;

/*
 * One per device slot. Lives in its own page.
 */
typedef struct xhci_dev {
    nq_u8 slot;
    nq_u8 speed;
    nq_u32 max_dci;

    nq_u8* out_ctx;
    nq_u8* in_ctx;
    nq_u8* ctl_buf;

    /*
     * State of the control transfer in flight (there is at most one).
     */
    nq_u64 data_trb;
    nq_u64 status_trb;
    volatile int ctl_done;
    nq_u32 ctl_code;
    nq_u32 ctl_residual;

    /*
     * Indexed by device context index (DCI): 1 is endpoint 0, then
     * 2 * endpoint number, plus 1 for IN.
     */
    xhci_ep_t ep[32];
} xhci_dev_t;

struct xhci64 {
    int running;

    pci64_device_t pci;

    nq_u64 mmio_phys;
    nq_u64 cap;
    nq_u64 op;
    nq_u64 rt;
    nq_u64 db;

    nq_u16 version;
    nq_u32 ctx_size;
    nq_u32 max_slots;
    nq_u32 max_ports;

    nq_u64* dcbaa;
    ring_t cmd;

    trb_t* evt;
    nq_u32 evt_deq;
    nq_u32 evt_cycle;

    cmd_wait_t* volatile cmd_wait;

    int port_event;
    int need_recover;

    nq_u8 port_usb3[XHCI64_MAX_PORTS + 1u];

    xhci_dev_t* dev[XHCI64_MAX_SLOTS + 1u];

    nq_u64 events_seen;
};

static struct xhci64 g_hc[XHCI64_MAX_CONTROLLERS];
static nq_u32 g_hc_count = 0;

/*
 * ------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------
 */

static nq_u32 r32(nq_u64 addr) {
    return *(volatile nq_u32*)addr;
}

static void w32(nq_u64 addr, nq_u32 value) {
    *(volatile nq_u32*)addr = value;
}

static void w64(nq_u64 addr, nq_u64 value) {
    w32(addr, (nq_u32)value);
    w32(addr + 4u, (nq_u32)(value >> 32));
}

static void zero_bytes(void* dst, nq_u64 count) {
    nq_u8* d = (nq_u8*)dst;

    for (nq_u64 i = 0; i < count; i++) {
        d[i] = 0;
    }
}

static void copy_bytes(void* dst, const void* src, nq_u64 count) {
    nq_u8* d = (nq_u8*)dst;
    const nq_u8* s = (const nq_u8*)src;

    for (nq_u64 i = 0; i < count; i++) {
        d[i] = s[i];
    }
}

/*
 * One zeroed page the controller may read and write.
 */
static void* dma_page(void) {
    nq_u64 frame = pmm64_alloc_frame();

    if (!frame) {
        return 0;
    }

    zero_bytes((void*)frame, 4096u);

    return (void*)frame;
}

static void dma_free(void* page) {
    if (page) {
        pmm64_free_frame((nq_u64)page);
    }
}

static nq_u32 g_usb_task_pid = 0;
static int g_usb_task_known = 0;

void xhci64_enter_task(void) {
    g_usb_task_pid = process64_current_pid();
    g_usb_task_known = 1;

    process64_preempt_disable();
}

static int in_usb_task(void) {
    return g_usb_task_known && process64_current_pid() == g_usb_task_pid;
}

void xhci64_task_sleep(nq_u32 ticks) {
    process64_preempt_enable();
    process64_sleep_ticks(ticks);
    process64_preempt_disable();
}

/*
 * One step of a wait: the USB task sleeps a tick (letting other tasks
 * run), anyone else just spins.
 */
static void wait_step(void) {
    if (in_usb_task()) {
        xhci64_task_sleep(1);
    } else {
        __asm__ volatile ("pause");
    }
}

static nq_u64 ms_to_ticks(nq_u32 ms) {
    nq_u64 hz = timer64_get_frequency();
    nq_u64 ticks;

    if (!hz) {
        hz = 1000u;
    }

    ticks = ((nq_u64)ms * hz + 999u) / 1000u;

    return ticks ? ticks : 1u;
}

/*
 * Waits for (register & mask) == want. Used only during bring-up, before
 * the event ring exists.
 */
static int wait_reg(nq_u64 addr, nq_u32 mask, nq_u32 want, nq_u32 ms) {
    nq_u64 start = timer64_get_ticks();
    nq_u64 limit = ms_to_ticks(ms);

    for (;;) {
        if ((r32(addr) & mask) == want) {
            return 1;
        }

        if ((timer64_get_ticks() - start) >= limit) {
            return 0;
        }

        wait_step();
    }
}

/*
 * ------------------------------------------------------------
 * Rings
 * ------------------------------------------------------------
 */

static int ring_init(ring_t* ring) {
    ring->trbs = (trb_t*)dma_page();
    ring->enqueue = 0;
    ring->cycle = 1;

    return ring->trbs != 0;
}

static void ring_free(ring_t* ring) {
    dma_free(ring->trbs);
    ring->trbs = 0;
}

/*
 * Appends one TRB and returns its address. The last entry of the page is a
 * link back to the start; passing it flips the cycle bit, which is how the
 * controller tells fresh TRBs from stale ones.
 */
static nq_u64 ring_push(ring_t* ring, nq_u32 p_lo, nq_u32 p_hi, nq_u32 status, nq_u32 control) {
    trb_t* trb = &ring->trbs[ring->enqueue];

    trb->p_lo = p_lo;
    trb->p_hi = p_hi;
    trb->status = status;
    trb->control = (control & ~TRB_CYCLE) | ring->cycle;

    ring->enqueue++;

    if (ring->enqueue == RING_TRBS - 1u) {
        trb_t* link = &ring->trbs[RING_TRBS - 1u];
        nq_u64 base = (nq_u64)ring->trbs;

        link->p_lo = (nq_u32)base;
        link->p_hi = (nq_u32)(base >> 32);
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE | ring->cycle;

        ring->enqueue = 0;
        ring->cycle ^= 1u;
    }

    return (nq_u64)trb;
}

static void doorbell(xhci64_t* hc, nq_u32 slot, nq_u32 target) {
    w32(hc->db + 4u * slot, target);
}

/*
 * ------------------------------------------------------------
 * Contexts
 * ------------------------------------------------------------
 */

static nq_u32* ctx_entry(xhci64_t* hc, nq_u8* base, nq_u32 index) {
    return (nq_u32*)(base + (nq_u64)index * hc->ctx_size);
}

static xhci_dev_t* slot_dev(xhci64_t* hc, nq_u8 slot) {
    if (!hc || slot == 0 || slot > XHCI64_MAX_SLOTS) {
        return 0;
    }

    return hc->dev[slot];
}

/*
 * ------------------------------------------------------------
 * Events
 * ------------------------------------------------------------
 */

static void ep_queue_in(xhci64_t* hc, xhci_dev_t* dev, nq_u32 dci) {
    xhci_ep_t* ep = &dev->ep[dci];
    nq_u64 buf = (nq_u64)ep->buffer;

    ring_push(
        &ep->ring,
        (nq_u32)buf,
        (nq_u32)(buf >> 32),
        ep->buffer_len,
        TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP
    );

    doorbell(hc, dev->slot, dci);
}

static void on_transfer_event(xhci64_t* hc, const trb_t* e) {
    nq_u8 slot = (nq_u8)(e->control >> 24);
    nq_u32 dci = (e->control >> 16) & 0x1Fu;
    nq_u32 code = e->status >> 24;
    nq_u32 residual = e->status & 0x00FFFFFFu;
    nq_u64 ptr = ((nq_u64)e->p_hi << 32) | e->p_lo;
    int ok = (code == CODE_SUCCESS || code == CODE_SHORT_PACKET);

    xhci_dev_t* dev = slot_dev(hc, slot);

    if (!dev || dci == 0) {
        return;
    }

    if (dci == 1u) {
        if (dev->ctl_done) {
            return;
        }

        if (ok && dev->data_trb && ptr == dev->data_trb) {
            /*
             * Data stage finished; the status stage still follows.
             */
            dev->ctl_residual = residual;
        } else if (ok && ptr == dev->status_trb) {
            dev->ctl_code = CODE_SUCCESS;
            dev->ctl_done = 1;
        } else if (!ok) {
            dev->ctl_code = code;
            dev->ctl_done = 1;
        }

        return;
    }

    xhci_ep_t* ep = &dev->ep[dci];

    if (!ep->active) {
        return;
    }

    if (ep->is_bulk) {
        if (!ep->xfer_done && (ptr == ep->wait_trb || !ok)) {
            ep->xfer_code = code;
            ep->xfer_residual = residual;
            ep->xfer_done = 1;
        }

        return;
    }

    if (ok) {
        nq_u32 length = ep->buffer_len;

        if (residual <= length) {
            length -= residual;
        }

        ep->errors = 0;

        if (ep->handler) {
            ep->handler(ep->handler_ctx, ep->buffer, length);
        }

        ep_queue_in(hc, dev, dci);
    } else {
        /*
         * The endpoint has stopped. Restarting it takes commands, which
         * cannot be issued from inside event handling: leave a note for
         * xhci64_recover().
         */
        ep->halted = 1;
        ep->errors++;
        hc->need_recover = 1;
    }
}

static void hc_events(xhci64_t* hc) {
    int handled = 0;

    if (!hc->running) {
        return;
    }

    for (nq_u32 n = 0; n < RING_TRBS * 2u; n++) {
        trb_t* e = &hc->evt[hc->evt_deq];

        if ((e->control & TRB_CYCLE) != hc->evt_cycle) {
            break;
        }

        nq_u32 type = (e->control >> 10) & 0x3Fu;

        if (type == TRB_EVENT_TRANSFER) {
            on_transfer_event(hc, e);
        } else if (type == TRB_EVENT_COMMAND) {
            nq_u64 ptr = ((nq_u64)e->p_hi << 32) | e->p_lo;

            cmd_wait_t* waiting = hc->cmd_wait;

            if (waiting && ptr == waiting->trb) {
                waiting->code = e->status >> 24;
                waiting->slot = (nq_u8)(e->control >> 24);
                waiting->done = 1;
                hc->cmd_wait = 0;
            }
        } else if (type == TRB_EVENT_PORT) {
            hc->port_event = 1;
        }

        hc->events_seen++;
        handled = 1;

        hc->evt_deq++;

        if (hc->evt_deq == RING_TRBS) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1u;
        }
    }

    if (handled) {
        /*
         * Tell the controller how far we have read; bit 3 clears its
         * "event handler busy" flag.
         */
        w64(hc->rt + IR0_ERDP, (nq_u64)&hc->evt[hc->evt_deq] | 0x8u);
    }
}

static void all_events(void) {
    for (nq_u32 i = 0; i < g_hc_count; i++) {
        hc_events(&g_hc[i]);
    }
}

void xhci64_idle_ms(nq_u32 ms) {
    nq_u64 start = timer64_get_ticks();
    nq_u64 limit = ms_to_ticks(ms);

    for (;;) {
        all_events();

        if ((timer64_get_ticks() - start) >= limit) {
            break;
        }

        wait_step();
    }
}

/*
 * Waits for a completion flag set by event handling.
 */
static int wait_done(volatile int* flag, nq_u32 ms) {
    nq_u64 start = timer64_get_ticks();
    nq_u64 limit = ms_to_ticks(ms);

    for (nq_u32 spins = 0;; spins++) {
        all_events();

        if (*flag) {
            return 1;
        }

        if ((timer64_get_ticks() - start) >= limit || spins >= SPIN_LIMIT) {
            return 0;
        }

        wait_step();
    }
}

/*
 * ------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------
 */

static int hc_command(xhci64_t* hc, nq_u64 param, nq_u32 control, nq_u8* out_slot) {
    cmd_wait_t wait;

    /*
     * The controller runs one command at a time. If the USB task is
     * asleep in the middle of one, let that finish first.
     */
    if (hc->cmd_wait) {
        nq_u64 start = timer64_get_ticks();
        nq_u64 limit = ms_to_ticks(COMMAND_TIMEOUT_MS);

        for (nq_u32 spins = 0; hc->cmd_wait; spins++) {
            all_events();

            if ((timer64_get_ticks() - start) >= limit || spins >= SPIN_LIMIT) {
                return 0;
            }

            wait_step();
        }
    }

    wait.done = 0;
    wait.code = CODE_TIMEOUT;
    wait.slot = 0;
    wait.trb = ring_push(&hc->cmd, (nq_u32)param, (nq_u32)(param >> 32), 0, control);

    hc->cmd_wait = &wait;

    doorbell(hc, 0, 0);

    if (!wait_done(&wait.done, COMMAND_TIMEOUT_MS)) {
        if (hc->cmd_wait == &wait) {
            hc->cmd_wait = 0;
        }

        return 0;
    }

    if (out_slot) {
        *out_slot = wait.slot;
    }

    return wait.code == CODE_SUCCESS;
}

/*
 * Gets a stopped or halted endpoint going again with an empty ring.
 */
static void ep_recover(xhci64_t* hc, xhci_dev_t* dev, nq_u32 dci) {
    xhci_ep_t* ep = &dev->ep[dci];
    nq_u32 state = ctx_entry(hc, dev->out_ctx, dci)[0] & 0x7u;
    nq_u32 target = (dci << 16) | ((nq_u32)dev->slot << 24);

    if (state == EP_STATE_HALTED) {
        hc_command(hc, 0, TRB_TYPE(TRB_RESET_EP) | target, 0);
    } else if (state == EP_STATE_RUNNING) {
        hc_command(hc, 0, TRB_TYPE(TRB_STOP_EP) | target, 0);
    }

    hc_command(
        hc,
        (nq_u64)&ep->ring.trbs[ep->ring.enqueue] | ep->ring.cycle,
        TRB_TYPE(TRB_SET_DEQUEUE) | target,
        0
    );
}

/*
 * ------------------------------------------------------------
 * Controller bring-up
 * ------------------------------------------------------------
 */

/*
 * Asks the firmware to let go of the controller. Until it does, the BIOS
 * may keep driving the ports for its own keyboard emulation.
 */
static void legacy_handoff(xhci64_t* hc, nq_u64 cap_addr) {
    nq_u32 value = r32(cap_addr);

    w32(cap_addr, value | (1u << 24));

    if (value & (1u << 16)) {
        if (!wait_reg(cap_addr, 1u << 16, 0, 1000u)) {
            usb64_log("USB: firmware did not release the controller; taking it anyway\n");
            w32(cap_addr, (r32(cap_addr) & ~(1u << 16)) | (1u << 24));
        }
    }

    /*
     * Switch off the firmware's SMI sources and acknowledge pending ones.
     */
    nq_u32 control = r32(cap_addr + 4u);

    control &= (0x7u << 1) | (0xFFu << 5) | (0x7u << 17);
    control |= 0x7u << 29;

    w32(cap_addr + 4u, control);

    (void)hc;
}

static void read_extended_caps(xhci64_t* hc, nq_u32 hcc1) {
    nq_u32 offset = (hcc1 >> 16) << 2;

    for (nq_u32 guard = 0; offset && guard < 64u; guard++) {
        if (offset + 16u > MMIO_MAP_SIZE) {
            break;
        }

        nq_u64 addr = hc->cap + offset;
        nq_u32 value = r32(addr);
        nq_u32 id = value & 0xFFu;
        nq_u32 next = (value >> 8) & 0xFFu;

        if (id == 1u) {
            legacy_handoff(hc, addr);
        } else if (id == 2u) {
            /*
             * "Supported protocol": which root ports are USB 2 and which
             * are USB 3.
             */
            nq_u32 major = value >> 24;
            nq_u32 ports = r32(addr + 8u);
            nq_u32 first = ports & 0xFFu;
            nq_u32 count = (ports >> 8) & 0xFFu;

            for (nq_u32 i = 0; i < count; i++) {
                nq_u32 port = first + i;

                if (port >= 1u && port <= XHCI64_MAX_PORTS) {
                    hc->port_usb3[port] = (major >= 3u) ? 1u : 0u;
                }
            }
        }

        if (!next) {
            break;
        }

        offset += next << 2;
    }
}

/*
 * Older Intel chipsets (7, 8 and 9 series) wire their USB 2 ports to an
 * EHCI controller by default and hand them to xHCI only on request.
 */
static void intel_route_ports(const pci64_device_t* dev) {
    if (dev->vendor_id != 0x8086u) {
        return;
    }

    if (dev->device_id != 0x1E31u && dev->device_id != 0x8C31u &&
        dev->device_id != 0x8CB1u && dev->device_id != 0x9C31u &&
        dev->device_id != 0x9CB1u) {
        return;
    }

    nq_u32 usb3_mask = pci64_config_read32(dev->bus, dev->device, dev->function, 0xDC);

    pci64_config_write32(dev->bus, dev->device, dev->function, 0xD8, usb3_mask);

    nq_u32 usb2_mask = pci64_config_read32(dev->bus, dev->device, dev->function, 0xD4);

    pci64_config_write32(dev->bus, dev->device, dev->function, 0xD0, usb2_mask);
}

static int hc_start(xhci64_t* hc, const pci64_device_t* pci) {
    int is_mmio = 0;
    int is_64 = 0;
    nq_u64 bar = pci64_read_bar(*pci, 0, &is_mmio, &is_64);

    if (!bar || !is_mmio) {
        usb64_log("USB: controller has no usable register block\n");
        return 0;
    }

    /*
     * Memory access and bus mastering on, legacy interrupt line off (we
     * poll).
     */
    nq_u16 command = pci64_config_read16(pci->bus, pci->device, pci->function, 0x04);

    command |= 0x0006u | 0x0400u;
    pci64_config_write16(pci->bus, pci->device, pci->function, 0x04, command);

    if (!paging64_map_direct_range(bar, MMIO_MAP_SIZE)) {
        usb64_log("USB: cannot map the controller registers\n");
        return 0;
    }

    hc->pci = *pci;
    hc->mmio_phys = bar;
    hc->cap = vmem64_phys_to_direct(bar);

    nq_u32 cap0 = r32(hc->cap + CAP_CAPLENGTH);
    nq_u32 hcs1 = r32(hc->cap + CAP_HCSPARAMS1);
    nq_u32 hcs2 = r32(hc->cap + CAP_HCSPARAMS2);
    nq_u32 hcc1 = r32(hc->cap + CAP_HCCPARAMS1);

    if (cap0 == 0xFFFFFFFFu) {
        usb64_log("USB: controller registers read as all ones\n");
        return 0;
    }

    hc->version = (nq_u16)(cap0 >> 16);
    hc->op = hc->cap + (cap0 & 0xFFu);
    hc->db = hc->cap + (r32(hc->cap + CAP_DBOFF) & ~0x3u);
    hc->rt = hc->cap + (r32(hc->cap + CAP_RTSOFF) & ~0x1Fu);
    hc->ctx_size = (hcc1 & 0x4u) ? 64u : 32u;

    hc->max_slots = hcs1 & 0xFFu;
    hc->max_ports = hcs1 >> 24;

    if (hc->max_slots > XHCI64_MAX_SLOTS) {
        hc->max_slots = XHCI64_MAX_SLOTS;
    }

    if (hc->max_ports > XHCI64_MAX_PORTS) {
        hc->max_ports = XHCI64_MAX_PORTS;
    }

    read_extended_caps(hc, hcc1);
    intel_route_ports(pci);

    /*
     * Stop, then reset.
     */
    w32(hc->op + OP_USBCMD, r32(hc->op + OP_USBCMD) & ~USBCMD_RUN);
    wait_reg(hc->op + OP_USBSTS, USBSTS_HALTED, USBSTS_HALTED, 100u);

    w32(hc->op + OP_USBCMD, USBCMD_RESET);

    if (!wait_reg(hc->op + OP_USBCMD, USBCMD_RESET, 0, 1000u) ||
        !wait_reg(hc->op + OP_USBSTS, USBSTS_NOT_READY, 0, 1000u)) {
        usb64_log("USB: controller did not come out of reset\n");
        return 0;
    }

    if (!(r32(hc->op + OP_PAGESIZE) & 0x1u)) {
        usb64_log("USB: controller does not support 4 KiB pages\n");
        return 0;
    }

    w32(hc->op + OP_CONFIG, hc->max_slots);

    /*
     * Device context base address array: slot ID -> device context.
     * Entry 0 points at the "scratchpad", pages the controller wants for
     * its own use.
     */
    hc->dcbaa = (nq_u64*)dma_page();

    if (!hc->dcbaa) {
        return 0;
    }

    nq_u32 scratch = (((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 27) & 0x1Fu);

    if (scratch) {
        nq_u64 frames = ((nq_u64)scratch * 8u + 4095u) / 4096u;
        nq_u64* list = (nq_u64*)pmm64_alloc_contiguous_frames(frames);

        if (!list) {
            return 0;
        }

        zero_bytes(list, frames * 4096u);

        for (nq_u32 i = 0; i < scratch; i++) {
            void* page = dma_page();

            if (!page) {
                return 0;
            }

            list[i] = (nq_u64)page;
        }

        hc->dcbaa[0] = (nq_u64)list;
    }

    w64(hc->op + OP_DCBAAP, (nq_u64)hc->dcbaa);

    if (!ring_init(&hc->cmd)) {
        return 0;
    }

    w64(hc->op + OP_CRCR, (nq_u64)hc->cmd.trbs | 1u);

    /*
     * Event ring: one segment, described by a one-entry segment table.
     */
    hc->evt = (trb_t*)dma_page();

    nq_u32* erst = (nq_u32*)dma_page();

    if (!hc->evt || !erst) {
        return 0;
    }

    hc->evt_deq = 0;
    hc->evt_cycle = 1;

    erst[0] = (nq_u32)(nq_u64)hc->evt;
    erst[1] = (nq_u32)((nq_u64)hc->evt >> 32);
    erst[2] = RING_TRBS;
    erst[3] = 0;

    w32(hc->rt + IR0_ERSTSZ, 1u);
    w64(hc->rt + IR0_ERDP, (nq_u64)hc->evt);
    w64(hc->rt + IR0_ERSTBA, (nq_u64)erst);

    /*
     * Interrupts stay off; acknowledge a pending one.
     */
    w32(hc->rt + IR0_IMAN, (r32(hc->rt + IR0_IMAN) & ~0x2u) | 0x1u);

    w32(hc->op + OP_USBCMD, USBCMD_RUN);

    if (!wait_reg(hc->op + OP_USBSTS, USBSTS_HALTED, 0, 200u)) {
        usb64_log("USB: controller did not start\n");
        return 0;
    }

    for (nq_u32 port = 1; port <= hc->max_ports; port++) {
        nq_u64 addr = hc->op + OP_PORTSC + 0x10u * (port - 1u);
        nq_u32 value = r32(addr);

        if (!(value & PORTSC_POWER)) {
            w32(addr, (value & PORTSC_PRESERVE) | PORTSC_POWER);
        }
    }

    hc->running = 1;
    hc->port_event = 1;

    return 1;
}

nq_u32 xhci64_init_all(void) {
    if (g_hc_count) {
        return g_hc_count;
    }

    for (nq_u32 n = 0; n < 16u && g_hc_count < XHCI64_MAX_CONTROLLERS; n++) {
        pci64_device_t pci;

        if (!pci64_find_class_nth(0x0Cu, 0x03u, 0x30u, n, &pci)) {
            break;
        }

        xhci64_t* hc = &g_hc[g_hc_count];

        usb64_log("USB: xHCI controller ");
        usb64_log_hex(pci.vendor_id, 4);
        usb64_log(":");
        usb64_log_hex(pci.device_id, 4);

        if (hc_start(hc, &pci)) {
            usb64_log(", ");
            usb64_log_dec(hc->max_ports);
            usb64_log(" ports\n");

            g_hc_count++;
        } else {
            usb64_log(" failed to start\n");

            /*
             * Start again with a clean record for the next controller.
             */
            zero_bytes(hc, sizeof(*hc));
        }
    }

    return g_hc_count;
}

nq_u32 xhci64_count(void) {
    return g_hc_count;
}

xhci64_t* xhci64_get(nq_u32 index) {
    return index < g_hc_count ? &g_hc[index] : 0;
}

void xhci64_poll(xhci64_t* hc) {
    if (hc) {
        hc_events(hc);
    }
}

void xhci64_recover(xhci64_t* hc) {
    if (!hc || !hc->running || !hc->need_recover) {
        return;
    }

    hc->need_recover = 0;

    for (nq_u32 slot = 1; slot <= XHCI64_MAX_SLOTS; slot++) {
        xhci_dev_t* dev = hc->dev[slot];

        if (!dev) {
            continue;
        }

        for (nq_u32 dci = 2; dci < 32u; dci++) {
            xhci_ep_t* ep = &dev->ep[dci];

            if (!ep->active || !ep->halted) {
                continue;
            }

            ep->halted = 0;

            if (ep->errors > EP_MAX_ERRORS) {
                ep->active = 0;
                continue;
            }

            ep_recover(hc, dev, dci);

            if (ep->handler) {
                ep_queue_in(hc, dev, dci);
            }
        }
    }
}

/*
 * ------------------------------------------------------------
 * Root hub ports
 * ------------------------------------------------------------
 */

static nq_u64 portsc_addr(xhci64_t* hc, nq_u32 port) {
    return hc->op + OP_PORTSC + 0x10u * (port - 1u);
}

nq_u32 xhci64_port_count(xhci64_t* hc) {
    return (hc && hc->running) ? hc->max_ports : 0;
}

int xhci64_port_is_usb3(xhci64_t* hc, nq_u32 port) {
    if (!hc || port == 0 || port > XHCI64_MAX_PORTS) {
        return 0;
    }

    return hc->port_usb3[port];
}

int xhci64_take_port_event(xhci64_t* hc) {
    if (!hc || !hc->port_event) {
        return 0;
    }

    hc->port_event = 0;

    return 1;
}

int xhci64_port_connected(xhci64_t* hc, nq_u32 port, int* out_changed) {
    if (out_changed) {
        *out_changed = 0;
    }

    if (!hc || !hc->running || port == 0 || port > hc->max_ports) {
        return 0;
    }

    nq_u64 addr = portsc_addr(hc, port);
    nq_u32 value = r32(addr);

    if (value == 0xFFFFFFFFu) {
        return 0;
    }

    if (value & PORTSC_CHANGES) {
        w32(addr, (value & PORTSC_PRESERVE) | (value & PORTSC_CHANGES));

        if (out_changed && (value & PORTSC_CSC)) {
            *out_changed = 1;
        }
    }

    return (value & PORTSC_CONNECTED) ? 1 : 0;
}

int xhci64_port_reset(xhci64_t* hc, nq_u32 port, nq_u8* out_speed) {
    if (!hc || !hc->running || port == 0 || port > hc->max_ports) {
        return 0;
    }

    nq_u64 addr = portsc_addr(hc, port);
    nq_u32 value = r32(addr);

    if (!(value & PORTSC_CONNECTED)) {
        return 0;
    }

    /*
     * A USB 3 port trains its link and enables itself; give it a moment
     * before falling back to a reset. USB 2 ports always need the reset.
     */
    int enabled = 0;

    if (hc->port_usb3[port]) {
        for (nq_u32 i = 0; i < 20u; i++) {
            value = r32(addr);

            if (value & PORTSC_ENABLED) {
                enabled = 1;
                break;
            }

            xhci64_idle_ms(10);
        }
    }

    if (!enabled) {
        value = r32(addr);
        w32(addr, (value & PORTSC_PRESERVE) | PORTSC_RESET);

        for (nq_u32 i = 0; i < 100u; i++) {
            xhci64_idle_ms(5);

            value = r32(addr);

            if (value & PORTSC_PRC) {
                break;
            }
        }

        value = r32(addr);
        w32(addr, (value & PORTSC_PRESERVE) | (value & PORTSC_CHANGES));

        /*
         * Reset recovery time.
         */
        xhci64_idle_ms(20);
    }

    value = r32(addr);

    if (!(value & PORTSC_ENABLED) || !(value & PORTSC_CONNECTED)) {
        return 0;
    }

    if (out_speed) {
        *out_speed = (nq_u8)((value >> 10) & 0xFu);
    }

    return 1;
}

/*
 * ------------------------------------------------------------
 * Devices
 * ------------------------------------------------------------
 */

static void dev_free(xhci64_t* hc, xhci_dev_t* dev) {
    if (!dev) {
        return;
    }

    for (nq_u32 dci = 1; dci < 32u; dci++) {
        ring_free(&dev->ep[dci].ring);
        dma_free(dev->ep[dci].buffer);
    }

    dma_free(dev->out_ctx);
    dma_free(dev->in_ctx);
    dma_free(dev->ctl_buf);
    dma_free(dev);

    (void)hc;
}

static nq_u32 default_ep0_packet(nq_u8 speed) {
    if (speed >= XHCI64_SPEED_SUPER) {
        return 512u;
    }

    if (speed == XHCI64_SPEED_HIGH) {
        return 64u;
    }

    /*
     * Low speed is always 8. Full speed can be 8..64; start with 8 and
     * let the caller correct it once the device descriptor says.
     */
    return 8u;
}

nq_u8 xhci64_device_open(xhci64_t* hc, const xhci64_device_info_t* info) {
    nq_u8 slot = 0;

    if (!hc || !hc->running || !info) {
        return 0;
    }

    if (!hc_command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) || slot == 0) {
        usb64_log("USB: controller refused a device slot\n");
        return 0;
    }

    if (slot > XHCI64_MAX_SLOTS) {
        hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((nq_u32)slot << 24), 0);
        return 0;
    }

    xhci_dev_t* dev = (xhci_dev_t*)dma_page();

    if (!dev) {
        hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((nq_u32)slot << 24), 0);
        return 0;
    }

    dev->slot = slot;
    dev->speed = info->speed;
    dev->max_dci = 1;
    dev->ctl_done = 1;

    dev->out_ctx = (nq_u8*)dma_page();
    dev->in_ctx = (nq_u8*)dma_page();
    dev->ctl_buf = (nq_u8*)dma_page();

    if (!dev->out_ctx || !dev->in_ctx || !dev->ctl_buf || !ring_init(&dev->ep[1].ring)) {
        hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((nq_u32)slot << 24), 0);
        dev_free(hc, dev);
        return 0;
    }

    hc->dcbaa[slot] = (nq_u64)dev->out_ctx;

    /*
     * Input context: entry 0 says which contexts follow (slot and
     * endpoint 0), entry 1 is the slot, entry 2 is endpoint 0.
     */
    nq_u32* control = ctx_entry(hc, dev->in_ctx, 0);
    nq_u32* slot_ctx = ctx_entry(hc, dev->in_ctx, 1);
    nq_u32* ep0 = ctx_entry(hc, dev->in_ctx, 2);
    nq_u64 ring = (nq_u64)dev->ep[1].ring.trbs;

    control[1] = 0x3u;

    slot_ctx[0] = (info->route & 0xFFFFFu) | ((nq_u32)info->speed << 20) | (1u << 27);
    slot_ctx[1] = (nq_u32)info->root_port << 16;
    slot_ctx[2] = (nq_u32)info->tt_slot | ((nq_u32)info->tt_port << 8);

    ep0[1] = (3u << 1) | (EP_TYPE_CONTROL << 3) | (default_ep0_packet(info->speed) << 16);
    ep0[2] = (nq_u32)ring | 1u;
    ep0[3] = (nq_u32)(ring >> 32);
    ep0[4] = 8u;

    hc->dev[slot] = dev;
    dev->ep[1].active = 1;

    if (!hc_command(
            hc,
            (nq_u64)dev->in_ctx,
            TRB_TYPE(TRB_ADDRESS_DEVICE) | ((nq_u32)slot << 24),
            0)) {
        xhci64_device_close(hc, slot);
        return 0;
    }

    return slot;
}

void xhci64_device_close(xhci64_t* hc, nq_u8 slot) {
    xhci_dev_t* dev = slot_dev(hc, slot);

    if (!dev) {
        return;
    }

    /*
     * Unhook first so late events for this slot are ignored.
     */
    hc->dev[slot] = 0;

    hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((nq_u32)slot << 24), 0);

    hc->dcbaa[slot] = 0;

    dev_free(hc, dev);
}

int xhci64_set_ep0_max_packet(xhci64_t* hc, nq_u8 slot, nq_u32 max_packet) {
    xhci_dev_t* dev = slot_dev(hc, slot);

    if (!dev) {
        return 0;
    }

    zero_bytes(dev->in_ctx, 4096u);

    nq_u32* control = ctx_entry(hc, dev->in_ctx, 0);
    nq_u32* in_ep0 = ctx_entry(hc, dev->in_ctx, 2);
    nq_u32* out_ep0 = ctx_entry(hc, dev->out_ctx, 1);

    control[1] = 0x2u;

    in_ep0[1] = (out_ep0[1] & 0x0000FFFFu) | (max_packet << 16);
    in_ep0[2] = out_ep0[2];
    in_ep0[3] = out_ep0[3];
    in_ep0[4] = out_ep0[4];

    return hc_command(
        hc,
        (nq_u64)dev->in_ctx,
        TRB_TYPE(TRB_EVALUATE_CTX) | ((nq_u32)slot << 24),
        0
    );
}

int xhci64_control(
    xhci64_t* hc,
    nq_u8 slot,
    nq_u8 request_type,
    nq_u8 request,
    nq_u16 value,
    nq_u16 index,
    void* data,
    nq_u16 length
) {
    xhci_dev_t* dev = slot_dev(hc, slot);

    if (!dev || length > 4096u || (length && !data)) {
        return -1;
    }

    xhci_ep_t* ep = &dev->ep[1];
    int in = (request_type & 0x80u) != 0;
    nq_u64 buf = (nq_u64)dev->ctl_buf;

    if (length && !in) {
        copy_bytes(dev->ctl_buf, data, length);
    }

    dev->data_trb = 0;
    dev->status_trb = 0;
    dev->ctl_residual = 0;
    dev->ctl_code = CODE_TIMEOUT;

    /*
     * Three stages: setup (the 8-byte request, carried inside the TRB),
     * optional data, and a status handshake in the opposite direction.
     */
    nq_u32 transfer_type = length ? (in ? 3u : 2u) : 0u;

    ring_push(
        &ep->ring,
        (nq_u32)request_type | ((nq_u32)request << 8) | ((nq_u32)value << 16),
        (nq_u32)index | ((nq_u32)length << 16),
        8u,
        TRB_TYPE(TRB_SETUP) | TRB_IDT | (transfer_type << 16)
    );

    nq_u64 data_trb = 0;

    if (length) {
        data_trb = ring_push(
            &ep->ring,
            (nq_u32)buf,
            (nq_u32)(buf >> 32),
            length,
            TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0u) | TRB_IOC | TRB_ISP
        );
    }

    nq_u64 status_trb = ring_push(
        &ep->ring,
        0,
        0,
        0,
        TRB_TYPE(TRB_STATUS) | ((length && in) ? 0u : TRB_DIR_IN) | TRB_IOC
    );

    dev->data_trb = data_trb;
    dev->status_trb = status_trb;
    dev->ctl_done = 0;

    doorbell(hc, slot, 1);

    int finished = wait_done(&dev->ctl_done, CONTROL_TIMEOUT_MS);

    dev->ctl_done = 1;

    if (!finished || dev->ctl_code != CODE_SUCCESS) {
        /*
         * A refused request (STALL) halts endpoint 0, and a timeout leaves
         * stale TRBs on its ring. Either way, reset it for the next one.
         */
        if (slot_dev(hc, slot) == dev) {
            ep_recover(hc, dev, 1);
        }

        return -1;
    }

    nq_u32 moved = length;

    if (dev->ctl_residual <= moved) {
        moved -= dev->ctl_residual;
    }

    if (moved && in) {
        copy_bytes(data, dev->ctl_buf, moved);
    }

    return (int)moved;
}

/*
 * Converts bInterval to the controller's unit: 2^n * 125 microseconds.
 */
static nq_u32 ep_interval(nq_u8 speed, nq_u8 interval) {
    if (speed == XHCI64_SPEED_HIGH || speed >= XHCI64_SPEED_SUPER) {
        nq_u32 n = interval ? (nq_u32)interval - 1u : 0u;

        return n > 15u ? 15u : n;
    }

    /*
     * Low/full speed: bInterval is in milliseconds (8 microframes each).
     */
    nq_u32 microframes = (nq_u32)(interval ? interval : 1u) * 8u;
    nq_u32 n = 0;

    while ((2u << n) <= microframes && n < 15u) {
        n++;
    }

    if (n < 3u) {
        n = 3u;
    }

    if (n > 10u) {
        n = 10u;
    }

    return n;
}

int xhci64_configure(
    xhci64_t* hc,
    nq_u8 slot,
    const xhci64_endpoint_t* endpoints,
    nq_u32 count,
    int is_hub,
    nq_u32 hub_ports,
    nq_u32 hub_ttt
) {
    xhci_dev_t* dev = slot_dev(hc, slot);

    if (!dev) {
        return 0;
    }

    zero_bytes(dev->in_ctx, 4096u);

    nq_u32* control = ctx_entry(hc, dev->in_ctx, 0);
    nq_u32* in_slot = ctx_entry(hc, dev->in_ctx, 1);
    nq_u32* out_slot = ctx_entry(hc, dev->out_ctx, 0);
    nq_u32 add = 0x1u;
    nq_u32 max_dci = dev->max_dci;

    for (nq_u32 i = 0; i < count; i++) {
        const xhci64_endpoint_t* desc = &endpoints[i];
        nq_u32 number = desc->address & 0x0Fu;
        int in = (desc->address & 0x80u) != 0;
        nq_u32 dci = number * 2u + (in ? 1u : 0u);

        /*
         * Interrupt IN (keyboards, mice) and bulk in either direction
         * (storage).
         */
        nq_u32 transfer = desc->attributes & 0x3u;
        int is_interrupt_in = (transfer == 3u) && in;
        int is_bulk = (transfer == 2u);

        if ((!is_interrupt_in && !is_bulk) || dci < 2u || dci > 31u) {
            continue;
        }

        xhci_ep_t* ep = &dev->ep[dci];

        if (ep->ring.trbs) {
            continue;
        }

        nq_u32 packet = desc->max_packet & 0x7FFu;

        if (packet == 0) {
            continue;
        }

        if (is_interrupt_in) {
            ep->buffer = (nq_u8*)dma_page();
            ep->buffer_len = packet;

            if (!ep->buffer) {
                return 0;
            }
        }

        if (!ring_init(&ep->ring)) {
            return 0;
        }

        ep->is_bulk = is_bulk;
        ep->xfer_done = 1;

        nq_u32* ctx = ctx_entry(hc, dev->in_ctx, dci + 1u);
        nq_u64 ring = (nq_u64)ep->ring.trbs;
        nq_u32 type = is_interrupt_in
            ? EP_TYPE_INTERRUPT_IN
            : (in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT);

        ctx[0] = is_interrupt_in ? (ep_interval(dev->speed, desc->interval) << 16) : 0u;
        ctx[1] = (3u << 1) | (type << 3) | (packet << 16);
        ctx[2] = (nq_u32)ring | 1u;
        ctx[3] = (nq_u32)(ring >> 32);
        ctx[4] = is_interrupt_in ? (packet | (packet << 16)) : 3072u;

        add |= 1u << dci;

        if (dci > max_dci) {
            max_dci = dci;
        }
    }

    control[1] = add;

    in_slot[0] = (out_slot[0] & 0x07FFFFFFu) | (max_dci << 27);
    in_slot[1] = out_slot[1];
    in_slot[2] = out_slot[2];

    if (is_hub) {
        in_slot[0] |= 1u << 26;
        in_slot[1] = (in_slot[1] & 0x00FFFFFFu) | ((hub_ports & 0xFFu) << 24);
        in_slot[2] = (in_slot[2] & ~(0x3u << 16)) | ((hub_ttt & 0x3u) << 16);
    }

    if (!hc_command(
            hc,
            (nq_u64)dev->in_ctx,
            TRB_TYPE(TRB_CONFIGURE_EP) | ((nq_u32)slot << 24),
            0)) {
        return 0;
    }

    dev->max_dci = max_dci;

    for (nq_u32 dci = 2; dci < 32u; dci++) {
        if (add & (1u << dci)) {
            dev->ep[dci].active = 1;
        }
    }

    return 1;
}

int xhci64_interrupt_in_start(
    xhci64_t* hc,
    nq_u8 slot,
    nq_u8 endpoint_address,
    xhci64_in_handler_t handler,
    void* ctx
) {
    xhci_dev_t* dev = slot_dev(hc, slot);
    nq_u32 dci = (nq_u32)(endpoint_address & 0x0Fu) * 2u + 1u;

    if (!dev || dci < 2u || dci > 31u || !dev->ep[dci].active ||
        dev->ep[dci].is_bulk || dev->ep[dci].handler) {
        return 0;
    }

    dev->ep[dci].handler = handler;
    dev->ep[dci].handler_ctx = ctx;

    ep_queue_in(hc, dev, dci);

    return 1;
}

int xhci64_bulk(
    xhci64_t* hc,
    nq_u8 slot,
    nq_u8 endpoint_address,
    void* dma,
    nq_u32 length,
    nq_u32* out_moved
) {
    xhci_dev_t* dev = slot_dev(hc, slot);
    nq_u32 dci = (nq_u32)(endpoint_address & 0x0Fu) * 2u +
                 ((endpoint_address & 0x80u) ? 1u : 0u);

    if (out_moved) {
        *out_moved = 0;
    }

    if (!dev || dci < 2u || dci > 31u || !dma || length == 0 || length > 0x10000u) {
        return 0;
    }

    xhci_ep_t* ep = &dev->ep[dci];

    if (!ep->active || !ep->is_bulk) {
        return 0;
    }

    nq_u64 buf = (nq_u64)dma;

    ep->xfer_code = CODE_TIMEOUT;
    ep->xfer_residual = 0;
    ep->wait_trb = ring_push(
        &ep->ring,
        (nq_u32)buf,
        (nq_u32)(buf >> 32),
        length,
        TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP
    );
    ep->xfer_done = 0;

    doorbell(hc, slot, dci);

    int finished = wait_done(&ep->xfer_done, BULK_TIMEOUT_MS);

    ep->xfer_done = 1;

    if (slot_dev(hc, slot) != dev) {
        return 0;
    }

    if (finished &&
        (ep->xfer_code == CODE_SUCCESS || ep->xfer_code == CODE_SHORT_PACKET)) {
        nq_u32 moved = length;

        if (ep->xfer_residual <= moved) {
            moved -= ep->xfer_residual;
        }

        if (out_moved) {
            *out_moved = moved;
        }

        return 1;
    }

    int stalled = finished && ep->xfer_code == CODE_STALL;

    ep_recover(hc, dev, dci);

    return stalled ? -1 : 0;
}

void xhci64_print_status(void) {
    if (!g_hc_count) {
        k64_print("No xHCI controller running.\n");
        return;
    }

    for (nq_u32 i = 0; i < g_hc_count; i++) {
        xhci64_t* hc = &g_hc[i];
        nq_u32 connected = 0;

        for (nq_u32 port = 1; port <= hc->max_ports; port++) {
            if (r32(portsc_addr(hc, port)) & PORTSC_CONNECTED) {
                connected++;
            }
        }

        k64_print("Controller ");
        k64_print_dec64(i);
        k64_print(": PCI ");
        k64_print_dec64(hc->pci.bus);
        k64_print(":");
        k64_print_dec64(hc->pci.device);
        k64_print(".");
        k64_print_dec64(hc->pci.function);
        k64_print("  ports ");
        k64_print_dec64(hc->max_ports);
        k64_print(" (");
        k64_print_dec64(connected);
        k64_print(" in use)  events ");
        k64_print_dec64(hc->events_seen);
        k64_print("\n");
    }
}
