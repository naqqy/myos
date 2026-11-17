#include "mouse64.h"

#include "../arch/x86_64/io64.h"

/*
 * PS/2 mouse driver (standard 3-byte packets, no scroll wheel).
 *
 * The mouse shares the 8042 controller with the keyboard. Its bytes arrive
 * on IRQ12 and are flagged with the AUX bit in the status register; irq64.c
 * reads the controller and hands those bytes to mouse64_handle_byte().
 */

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02

#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_ENABLE_AUX    0xA8
#define PS2_CMD_WRITE_AUX     0xD4

#define PS2_CONFIG_IRQ12        0x02
#define PS2_CONFIG_AUX_DISABLED 0x20

#define MOUSE_CMD_SET_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE       0xF4
#define MOUSE_ACK              0xFA

#define MOUSE64_QUEUE_SIZE 128u

/*
 * VMware "backdoor" absolute pointer (vmmouse).
 *
 * Hypervisors that implement it (QEMU, VMware, VirtualBox) let the guest
 * ask for the host pointer position through a magic I/O port. With it the
 * guest cursor follows the host cursor exactly and the VM window does not
 * need to grab the mouse. On real hardware the port does not answer and we
 * stay with plain PS/2 packets.
 */
#define VMMOUSE_PORT  0x5658
#define VMMOUSE_MAGIC 0x564D5868u

#define VMMOUSE_CMD_GETVERSION        10u
#define VMMOUSE_CMD_ABSPOINTER_DATA   39u
#define VMMOUSE_CMD_ABSPOINTER_STATUS 40u
#define VMMOUSE_CMD_ABSPOINTER_COMMAND 41u

#define VMMOUSE_ENABLE           0x45414552u
#define VMMOUSE_REQUEST_ABSOLUTE 0x53424152u
#define VMMOUSE_VERSION_ID       0x3442554Au
#define VMMOUSE_ERROR            0xFFFF0000u

#define VMMOUSE_BUTTON_LEFT   0x20u
#define VMMOUSE_BUTTON_RIGHT  0x10u
#define VMMOUSE_BUTTON_MIDDLE 0x08u

/*
 * Polling budget for controller handshakes. Large enough for slow real
 * controllers, small enough that a machine without a PS/2 port does not
 * stall the boot.
 */
#define PS2_WAIT_SPINS 100000u

static int g_mouse_available = 0;
static int g_mouse_absolute = 0;

static nq_u8 g_packet[3];
static nq_u32 g_packet_index = 0;

static volatile mouse64_event_t g_queue[MOUSE64_QUEUE_SIZE];
static volatile nq_u32 g_queue_head = 0;
static volatile nq_u32 g_queue_tail = 0;

static nq_u64 irq_save_disable(void) {
    nq_u64 flags;

    __asm__ volatile (
        "pushfq; pop %0; cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void irq_restore(nq_u64 flags) {
    if (flags & 0x200ull) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

/*
 * One backdoor call. Registers in: eax = magic, ebx = argument,
 * ecx = command, edx = port. All four come back with results.
 */
static void vmmouse_call(
    nq_u32 command,
    nq_u32 argument,
    nq_u32* out_eax,
    nq_u32* out_ebx,
    nq_u32* out_ecx,
    nq_u32* out_edx
) {
    nq_u32 eax = VMMOUSE_MAGIC;
    nq_u32 ebx = argument;
    nq_u32 ecx = command;
    nq_u32 edx = VMMOUSE_PORT;

    __asm__ volatile (
        "inl %%dx, %%eax"
        : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx)
    );

    if (out_eax) {
        *out_eax = eax;
    }

    if (out_ebx) {
        *out_ebx = ebx;
    }

    if (out_ecx) {
        *out_ecx = ecx;
    }

    if (out_edx) {
        *out_edx = edx;
    }
}

/*
 * Switches the hypervisor's pointer to absolute mode.
 * Returns 0 on real hardware or if the hypervisor has no vmmouse.
 */
static int vmmouse_enable(void) {
    nq_u32 eax = 0;
    nq_u32 ebx = 0;

    /*
     * A hypervisor answers GETVERSION by writing the magic into ebx.
     */
    vmmouse_call(VMMOUSE_CMD_GETVERSION, ~VMMOUSE_MAGIC, &eax, &ebx, 0, 0);

    if (ebx != VMMOUSE_MAGIC || eax == 0xFFFFFFFFu) {
        return 0;
    }

    vmmouse_call(VMMOUSE_CMD_ABSPOINTER_COMMAND, VMMOUSE_ENABLE, 0, 0, 0, 0);
    vmmouse_call(VMMOUSE_CMD_ABSPOINTER_STATUS, 0, &eax, 0, 0, 0);

    if ((eax & 0xFFFFu) == 0 || (eax & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
        return 0;
    }

    /*
     * Enabling queues one word: the protocol version.
     */
    vmmouse_call(VMMOUSE_CMD_ABSPOINTER_DATA, 1, &eax, 0, 0, 0);

    if (eax != VMMOUSE_VERSION_ID) {
        return 0;
    }

    vmmouse_call(
        VMMOUSE_CMD_ABSPOINTER_COMMAND,
        VMMOUSE_REQUEST_ABSOLUTE,
        0, 0, 0, 0
    );

    return 1;
}

static void queue_push(const mouse64_event_t* ev);

/*
 * Reads every pending absolute update from the hypervisor.
 */
static void vmmouse_drain(void) {
    for (int i = 0; i < 64; i++) {
        nq_u32 status = 0;
        nq_u32 flags = 0;
        nq_u32 x = 0;
        nq_u32 y = 0;

        vmmouse_call(VMMOUSE_CMD_ABSPOINTER_STATUS, 0, &status, 0, 0, 0);

        if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
            /*
             * The hypervisor dropped out of vmmouse mode (for example after
             * a device reset); ask for it again.
             */
            g_mouse_absolute = vmmouse_enable();
            return;
        }

        /*
         * Updates are four words each: flags/buttons, x, y, wheel.
         */
        if ((status & 0xFFFFu) < 4) {
            return;
        }

        vmmouse_call(VMMOUSE_CMD_ABSPOINTER_DATA, 4, &flags, &x, &y, 0);

        mouse64_event_t ev;

        ev.dx = 0;
        ev.dy = 0;
        ev.buttons = 0;
        ev.absolute = 1;
        ev.abs_x = x & 0xFFFFu;
        ev.abs_y = y & 0xFFFFu;

        if (flags & VMMOUSE_BUTTON_LEFT) {
            ev.buttons |= MOUSE64_BUTTON_LEFT;
        }

        if (flags & VMMOUSE_BUTTON_RIGHT) {
            ev.buttons |= MOUSE64_BUTTON_RIGHT;
        }

        if (flags & VMMOUSE_BUTTON_MIDDLE) {
            ev.buttons |= MOUSE64_BUTTON_MIDDLE;
        }

        queue_push(&ev);
    }
}

static int ps2_wait_can_write(void) {
    for (nq_u32 i = 0; i < PS2_WAIT_SPINS; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
            return 1;
        }
    }

    return 0;
}

static int ps2_wait_can_read(void) {
    for (nq_u32 i = 0; i < PS2_WAIT_SPINS; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return 1;
        }
    }

    return 0;
}

static void ps2_flush_output(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
            break;
        }

        (void)inb(PS2_DATA_PORT);
    }
}

static int ps2_command(nq_u8 command) {
    if (!ps2_wait_can_write()) {
        return 0;
    }

    outb(PS2_COMMAND_PORT, command);

    return 1;
}

/*
 * Sends one command byte to the mouse and waits for its ACK.
 */
static int mouse_command(nq_u8 command) {
    if (!ps2_command(PS2_CMD_WRITE_AUX)) {
        return 0;
    }

    if (!ps2_wait_can_write()) {
        return 0;
    }

    outb(PS2_DATA_PORT, command);

    /*
     * The ACK can be preceded by stale bytes; look at a few.
     */
    for (int i = 0; i < 8; i++) {
        if (!ps2_wait_can_read()) {
            return 0;
        }

        if (inb(PS2_DATA_PORT) == MOUSE_ACK) {
            return 1;
        }
    }

    return 0;
}

int mouse64_init(void) {
    nq_u8 config;

    g_mouse_available = 0;
    g_packet_index = 0;
    g_queue_head = 0;
    g_queue_tail = 0;

    ps2_flush_output();

    if (!ps2_command(PS2_CMD_ENABLE_AUX)) {
        return 0;
    }

    /*
     * Turn on the mouse clock and its interrupt in the controller
     * configuration byte. Everything else (keyboard IRQ, scancode
     * translation) is left exactly as the firmware set it.
     */
    if (!ps2_command(PS2_CMD_READ_CONFIG) || !ps2_wait_can_read()) {
        return 0;
    }

    config = inb(PS2_DATA_PORT);
    config |= PS2_CONFIG_IRQ12;
    config &= (nq_u8)~PS2_CONFIG_AUX_DISABLED;

    if (!ps2_command(PS2_CMD_WRITE_CONFIG) || !ps2_wait_can_write()) {
        return 0;
    }

    outb(PS2_DATA_PORT, config);

    if (!mouse_command(MOUSE_CMD_SET_DEFAULTS)) {
        ps2_flush_output();
        return 0;
    }

    if (!mouse_command(MOUSE_CMD_ENABLE)) {
        ps2_flush_output();
        return 0;
    }

    ps2_flush_output();

    g_mouse_available = 1;

    /*
     * In a VM, prefer the absolute pointer. The PS/2 packets keep arriving
     * (they are what raises the interrupt) but their deltas are ignored.
     */
    g_mouse_absolute = vmmouse_enable();

    return 1;
}

int mouse64_available(void) {
    return g_mouse_available;
}

int mouse64_is_absolute(void) {
    return g_mouse_absolute;
}

/*
 * Adds an event to the queue. Called with interrupts off (IRQ context).
 */
static void queue_push(const mouse64_event_t* ev) {
    nq_u32 next = (g_queue_tail + 1u) % MOUSE64_QUEUE_SIZE;

    if (next == g_queue_head) {
        /*
         * Queue full: fold this update into the newest queued one so no
         * movement is lost and the latest position and buttons win.
         */
        nq_u32 last =
            (g_queue_tail + MOUSE64_QUEUE_SIZE - 1u) % MOUSE64_QUEUE_SIZE;

        g_queue[last].dx += ev->dx;
        g_queue[last].dy += ev->dy;
        g_queue[last].buttons = ev->buttons;
        g_queue[last].absolute = ev->absolute;
        g_queue[last].abs_x = ev->abs_x;
        g_queue[last].abs_y = ev->abs_y;
        return;
    }

    g_queue[g_queue_tail].dx = ev->dx;
    g_queue[g_queue_tail].dy = ev->dy;
    g_queue[g_queue_tail].buttons = ev->buttons;
    g_queue[g_queue_tail].absolute = ev->absolute;
    g_queue[g_queue_tail].abs_x = ev->abs_x;
    g_queue[g_queue_tail].abs_y = ev->abs_y;
    g_queue_tail = next;
}

void mouse64_handle_byte(nq_u8 data) {
    if (!g_mouse_available) {
        return;
    }

    /*
     * Bit 3 of the first byte is always set. If it is not, we are out of
     * step with the packet stream: drop bytes until one that can start a
     * packet arrives.
     */
    if (g_packet_index == 0 && !(data & 0x08)) {
        return;
    }

    g_packet[g_packet_index++] = data;

    if (g_packet_index < 3) {
        return;
    }

    g_packet_index = 0;

    /*
     * Absolute mode: the packet only signals "something changed"; the real
     * position and buttons come from the hypervisor.
     */
    if (g_mouse_absolute) {
        vmmouse_drain();
        return;
    }

    nq_u8 flags = g_packet[0];

    /*
     * Overflow bits set: the deltas are meaningless.
     */
    if (flags & 0xC0) {
        return;
    }

    mouse64_event_t ev;

    /*
     * PS/2 reports +Y as "up"; the screen grows downward.
     */
    ev.dx = (int)g_packet[1] - ((flags & 0x10) ? 256 : 0);
    ev.dy = -((int)g_packet[2] - ((flags & 0x20) ? 256 : 0));
    ev.buttons = (nq_u8)(flags & 0x07);
    ev.absolute = 0;
    ev.abs_x = 0;
    ev.abs_y = 0;

    queue_push(&ev);
}

int mouse64_poll(mouse64_event_t* out) {
    if (!out) {
        return 0;
    }

    nq_u64 flags = irq_save_disable();

    /*
     * Absolute mode does not depend on the PS/2 interrupt alone: ask the
     * hypervisor directly whenever our queue is empty.
     */
    if (g_mouse_absolute && g_queue_head == g_queue_tail) {
        vmmouse_drain();
    }

    if (g_queue_head == g_queue_tail) {
        irq_restore(flags);
        return 0;
    }

    out->dx = g_queue[g_queue_head].dx;
    out->dy = g_queue[g_queue_head].dy;
    out->buttons = g_queue[g_queue_head].buttons;
    out->absolute = g_queue[g_queue_head].absolute;
    out->abs_x = g_queue[g_queue_head].abs_x;
    out->abs_y = g_queue[g_queue_head].abs_y;

    g_queue_head = (g_queue_head + 1u) % MOUSE64_QUEUE_SIZE;

    irq_restore(flags);

    return 1;
}
