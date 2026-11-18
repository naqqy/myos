#include "bt64.h"
#include "bt_internal64.h"

#include "../k64_console.h"
#include "../arch/x86_64/timer64.h"
#include "../proc/process64.h"
#include "../mm/pmm64.h"
#include "../fs/vfs64.h"

/*
 * Bluetooth: USB transport, the controller, and the link to a headset.
 * See bt64.h, and bta2dp64.c for what runs over the link.
 *
 * One kernel task ("bt") does all the work. The USB task only drops
 * arriving events and data packets into queues; the shell only sets a
 * request and watches the trace. So the stack itself is single-threaded.
 *
 * Locking follows the USB rule: every call into the xHCI driver is made
 * with task switching off, and never held while waiting.
 */

#define EVENT_MAX        260u
#define EVENT_QUEUE      16u
#define ACL_MAX          1100u
#define ACL_QUEUE        12u
#define LOG_SIZE         8192u
#define FOUND_MAX        12u

#define USB_VENDOR_INTEL 0x8087u
#define KEY_FILE         "/usb/NAQBT.CFG"

/* HCI commands: (group << 10) | number */
#define HCI_INQUIRY              0x0401u
#define HCI_INQUIRY_CANCEL       0x0402u
#define HCI_CREATE_CONNECTION    0x0405u
#define HCI_DISCONNECT           0x0406u
#define HCI_ACCEPT_CONNECTION    0x0409u
#define HCI_LINK_KEY_REPLY       0x040Bu
#define HCI_LINK_KEY_NEGATIVE    0x040Cu
#define HCI_PIN_REPLY            0x040Du
#define HCI_AUTH_REQUESTED       0x0411u
#define HCI_SET_ENCRYPTION       0x0413u
#define HCI_REMOTE_NAME          0x0419u
#define HCI_IO_CAPABILITY_REPLY  0x042Bu
#define HCI_CONFIRM_REPLY        0x042Cu
#define HCI_WRITE_LINK_POLICY    0x080Fu
#define HCI_SET_EVENT_MASK       0x0C01u
#define HCI_RESET                0x0C03u
#define HCI_WRITE_LOCAL_NAME     0x0C13u
#define HCI_WRITE_PAGE_TIMEOUT   0x0C18u
#define HCI_WRITE_SCAN_ENABLE    0x0C1Au
#define HCI_WRITE_CLASS          0x0C24u
#define HCI_WRITE_INQUIRY_MODE   0x0C45u
#define HCI_WRITE_SSP_MODE       0x0C56u
#define HCI_READ_LOCAL_VERSION   0x1001u
#define HCI_READ_BUFFER_SIZE     0x1005u
#define HCI_READ_BD_ADDR         0x1009u
#define HCI_INTEL_READ_VERSION   0xFC05u

/* HCI events */
#define EV_INQUIRY_COMPLETE      0x01u
#define EV_INQUIRY_RESULT        0x02u
#define EV_CONNECTION_COMPLETE   0x03u
#define EV_CONNECTION_REQUEST    0x04u
#define EV_DISCONNECTED          0x05u
#define EV_AUTH_COMPLETE         0x06u
#define EV_REMOTE_NAME           0x07u
#define EV_ENCRYPTION_CHANGE     0x08u
#define EV_COMMAND_COMPLETE      0x0Eu
#define EV_COMMAND_STATUS        0x0Fu
#define EV_PACKETS_DONE          0x13u
#define EV_PIN_REQUEST           0x16u
#define EV_LINK_KEY_REQUEST      0x17u
#define EV_LINK_KEY              0x18u
#define EV_INQUIRY_RESULT_RSSI   0x22u
#define EV_INQUIRY_RESULT_EXT    0x2Fu
#define EV_IO_CAPABILITY_REQUEST 0x31u
#define EV_CONFIRM_REQUEST       0x33u
#define EV_PAIRING_COMPLETE      0x36u

typedef struct event {
    nq_u32 length;
    nq_u8 data[EVENT_MAX];
} event_t;

typedef struct packet {
    nq_u32 length;
    nq_u8 data[ACL_MAX];
} packet_t;

typedef struct found {
    nq_u8 addr[6];
    nq_u8 cod[3];
    nq_u8 page_mode;
    nq_u16 clock;
    char name[32];
} found_t;

enum {
    CTRL_ABSENT,        /* no controller */
    CTRL_NEW,           /* attached, not set up yet */
    CTRL_NO_FIRMWARE,   /* waiting for a firmware file we cannot load yet */
    CTRL_FAILED,
    CTRL_READY
};

enum {
    LINK_NONE,
    LINK_CALLING,
    LINK_UP,            /* connected, not yet secured */
    LINK_SECURE         /* authenticated and encrypted */
};

enum {
    REQ_NONE,
    REQ_SCAN,
    REQ_CONNECT,        /* g_request_addr */
    REQ_DISCONNECT,
    REQ_FORGET
};

/* ---- transport ---- */

static int g_present = 0;
static xhci64_t* g_hc = 0;
static nq_u8 g_slot = 0;
static bt64_interface_t g_intf;
static nq_u16 g_vendor = 0;
static nq_u16 g_product = 0;

static nq_u8 g_event_partial[EVENT_MAX];
static nq_u32 g_event_partial_len = 0;
static event_t g_events[EVENT_QUEUE];
static nq_u32 g_event_head = 0;
static nq_u32 g_event_count = 0;

static nq_u8 g_acl_partial[ACL_MAX];
static nq_u32 g_acl_partial_len = 0;
static packet_t g_packets[ACL_QUEUE];
static nq_u32 g_packet_head = 0;
static nq_u32 g_packet_count = 0;
static nq_u64 g_dropped = 0;

static nq_u8* g_tx = 0;             /* one page for outgoing data packets */

/* ---- controller ---- */

static int g_task_started = 0;
static int g_ctrl = CTRL_ABSENT;
static nq_u8 g_own_addr[6];
static nq_u32 g_acl_mtu = 0;
static nq_u32 g_acl_slots = 0;      /* packets the controller can hold */
static nq_u32 g_acl_free = 0;
static char g_firmware[40] = "";

/* ---- link ---- */

static int g_link = LINK_NONE;
static nq_u32 g_handle = 0;
static nq_u8 g_peer[6];
static char g_peer_name[32] = "";
static int g_we_called = 0;
static int g_accepting = 0;         /* answering a call rather than making one */
static nq_u64 g_link_since = 0;
static int g_auth_asked = 0;

static int g_have_key = 0;
static nq_u8 g_key_addr[6];
static nq_u8 g_key[16];
static char g_key_name[32] = "";

/* things an event asked for, done by the task afterwards */
static int g_do_key_reply = 0;
static int g_do_io_reply = 0;
static int g_do_confirm = 0;
static int g_do_pin = 0;
static int g_do_accept = 0;
static int g_do_auth = 0;
static int g_do_encrypt = 0;
static int g_do_save = 0;
static nq_u8 g_ask_addr[6];

/* incoming data being put back together */
static nq_u8 g_frame[ACL_MAX];
static nq_u32 g_frame_len = 0;
static nq_u32 g_frame_want = 0;

/* ---- scanning ---- */

static found_t g_found[FOUND_MAX];
static nq_u32 g_found_count = 0;
static int g_scanning = 0;
static int g_name_pending = 0;
static nq_u8 g_name_addr[6];

/* ---- requests from the shell ---- */

static volatile int g_request = REQ_NONE;
static nq_u8 g_request_addr[6];
static volatile int g_busy = 0;     /* the task is carrying out a request */

/* ---- trace ---- */

static char g_log[LOG_SIZE];
static nq_u32 g_log_len = 0;
static nq_u64 g_log_total = 0;      /* characters ever written */

/* -------------------------------------------------------------------------- */
/* Trace                                                                       */
/* -------------------------------------------------------------------------- */

void bt_trace(const char* text) {
    process64_preempt_disable();

    for (; *text; text++) {
        if (g_log_len + 1u >= LOG_SIZE) {
            /* full: forget the older half */
            nq_u32 cut = LOG_SIZE / 2u;

            for (nq_u32 i = cut; i < g_log_len; i++) {
                g_log[i - cut] = g_log[i];
            }

            g_log_len -= cut;
        }

        g_log[g_log_len++] = *text;
        g_log_total++;
    }

    g_log[g_log_len] = 0;

    process64_preempt_enable();
}

void bt_trace_hex(nq_u32 value, nq_u32 digits) {
    char text[9];

    for (nq_u32 i = 0; i < digits && i < 8u; i++) {
        text[i] = "0123456789abcdef"[(value >> (4u * (digits - 1u - i))) & 0xFu];
    }

    text[digits < 8u ? digits : 8u] = 0;
    bt_trace(text);
}

void bt_trace_dec(nq_u64 value) {
    char digits[24];
    char text[24];
    nq_u32 d = 0;
    nq_u32 n = 0;

    do {
        digits[d++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);

    while (d) {
        text[n++] = digits[--d];
    }

    text[n] = 0;
    bt_trace(text);
}

static void trace_addr(const nq_u8* addr) {
    for (int i = 5; i >= 0; i--) {
        bt_trace_hex(addr[i], 2);

        if (i) {
            bt_trace(":");
        }
    }
}

nq_u64 bt_now_ms(void) {
    nq_u64 hz = timer64_get_frequency();

    return (timer64_get_ticks() * 1000u) / (hz ? hz : 1000u);
}

static void sleep_ms(nq_u32 ms) {
    nq_u64 hz = timer64_get_frequency();
    nq_u64 ticks = ((nq_u64)ms * (hz ? hz : 1000u) + 999u) / 1000u;

    process64_sleep_ticks(ticks ? ticks : 1u);
}

static int same_addr(const nq_u8* a, const nq_u8* b) {
    for (int i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }

    return 1;
}

static void copy_bytes(nq_u8* dst, const nq_u8* src, nq_u32 count) {
    for (nq_u32 i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

static void copy_name(char* dst, const char* src, nq_u32 size) {
    nq_u32 i = 0;

    for (; src[i] && i + 1u < size; i++) {
        dst[i] = ((nq_u8)src[i] < 32u || (nq_u8)src[i] > 126u) ? '?' : src[i];
    }

    dst[i] = 0;
}

/* -------------------------------------------------------------------------- */
/* USB transport                                                               */
/* -------------------------------------------------------------------------- */

/* Both handlers run in the USB task, with task switching off. */

static void on_event_packet(void* ctx, const nq_u8* data, nq_u32 length) {
    (void)ctx;

    for (nq_u32 i = 0; i < length; i++) {
        if (g_event_partial_len < EVENT_MAX) {
            g_event_partial[g_event_partial_len++] = data[i];
        }

        /* byte 1 of an event is the number of bytes that follow it */
        if (g_event_partial_len >= 2u && g_event_partial_len == 2u + g_event_partial[1]) {
            if (g_event_count < EVENT_QUEUE) {
                event_t* e = &g_events[(g_event_head + g_event_count) % EVENT_QUEUE];

                e->length = g_event_partial_len;
                copy_bytes(e->data, g_event_partial, g_event_partial_len);
                g_event_count++;
            } else {
                g_dropped++;
            }

            g_event_partial_len = 0;
        }
    }
}

static void on_data_packet(void* ctx, const nq_u8* data, nq_u32 length) {
    (void)ctx;

    for (nq_u32 i = 0; i < length; i++) {
        if (g_acl_partial_len < ACL_MAX) {
            g_acl_partial[g_acl_partial_len++] = data[i];
        }

        /* bytes 2..3 of a data packet are the number of bytes that follow */
        if (g_acl_partial_len >= 4u) {
            nq_u32 want = 4u + ((nq_u32)g_acl_partial[2] | ((nq_u32)g_acl_partial[3] << 8));

            if (want > ACL_MAX) {
                g_acl_partial_len = 0;      /* nonsense: start again */
                g_dropped++;
            } else if (g_acl_partial_len == want) {
                if (g_packet_count < ACL_QUEUE) {
                    packet_t* p = &g_packets[(g_packet_head + g_packet_count) % ACL_QUEUE];

                    p->length = want;
                    copy_bytes(p->data, g_acl_partial, want);
                    g_packet_count++;
                } else {
                    g_dropped++;
                }

                g_acl_partial_len = 0;
            }
        }
    }
}

int bt64_attach(xhci64_t* hc, nq_u8 slot, const bt64_interface_t* intf,
                nq_u16 vendor, nq_u16 product) {
    if (g_present || !intf->ep_events) {
        return 0;
    }

    if (!g_tx) {
        g_tx = (nq_u8*)pmm64_alloc_frame();

        if (!g_tx) {
            return 0;
        }
    }

    g_hc = hc;
    g_slot = slot;
    g_intf.number = intf->number;
    g_intf.ep_events = intf->ep_events;
    g_intf.ep_data_in = intf->ep_data_in;
    g_intf.ep_data_out = intf->ep_data_out;
    g_intf.events_packet = intf->events_packet;
    g_intf.data_in_packet = intf->data_in_packet;
    g_intf.data_out_packet = intf->data_out_packet;
    g_intf.events_interval = intf->events_interval;
    g_vendor = vendor;
    g_product = product;
    g_event_partial_len = 0;
    g_event_head = 0;
    g_event_count = 0;
    g_acl_partial_len = 0;
    g_packet_head = 0;
    g_packet_count = 0;

    if (!xhci64_interrupt_in_start(hc, slot, intf->ep_events, on_event_packet, 0)) {
        return 0;
    }

    if (!xhci64_bulk_in_listen(hc, slot, intf->ep_data_in, on_data_packet, 0)) {
        bt_trace("usb: could not listen for data packets\n");
    }

    g_present = 1;
    g_ctrl = CTRL_NEW;

    return 1;
}

void bt64_detach(xhci64_t* hc, nq_u8 slot) {
    if (g_present && g_hc == hc && g_slot == slot) {
        g_present = 0;
        g_hc = 0;
        g_ctrl = CTRL_ABSENT;
    }
}

int bt64_present(void) {
    return g_present;
}

static int event_take(event_t* out) {
    int got = 0;

    process64_preempt_disable();

    if (g_event_count) {
        const event_t* e = &g_events[g_event_head];

        out->length = e->length;
        copy_bytes(out->data, e->data, e->length);

        g_event_head = (g_event_head + 1u) % EVENT_QUEUE;
        g_event_count--;
        got = 1;
    }

    process64_preempt_enable();

    return got;
}

static int packet_take(packet_t* out) {
    int got = 0;

    process64_preempt_disable();

    if (g_packet_count) {
        const packet_t* p = &g_packets[g_packet_head];

        out->length = p->length;
        copy_bytes(out->data, p->data, p->length);

        g_packet_head = (g_packet_head + 1u) % ACL_QUEUE;
        g_packet_count--;
        got = 1;
    }

    process64_preempt_enable();

    return got;
}

nq_u32 bt_acl_room(void) {
    return g_acl_free;
}

int bt_acl_send(const nq_u8* frame, nq_u32 length) {
    if (!g_present || g_link < LINK_UP || !g_acl_mtu) {
        return 0;
    }

    nq_u32 pieces = (length + g_acl_mtu - 1u) / g_acl_mtu;

    if (pieces > g_acl_free) {
        return 0;
    }

    for (nq_u32 at = 0; at < length; ) {
        nq_u32 part = length - at;

        if (part > g_acl_mtu) {
            part = g_acl_mtu;
        }

        /* handle, plus "start of a frame" (2) or "continuation" (1) in bits 12..13 */
        nq_u32 head = g_handle | (at == 0 ? 0x2000u : 0x1000u);

        g_tx[0] = (nq_u8)(head & 0xFFu);
        g_tx[1] = (nq_u8)(head >> 8);
        g_tx[2] = (nq_u8)(part & 0xFFu);
        g_tx[3] = (nq_u8)(part >> 8);
        copy_bytes(g_tx + 4, frame + at, part);

        process64_preempt_disable();

        int ok = g_present ? xhci64_bulk(g_hc, g_slot, g_intf.ep_data_out, g_tx, part + 4u, 0) : 0;

        process64_preempt_enable();

        if (ok != 1) {
            return 0;
        }

        if (g_acl_free) {
            g_acl_free--;
        }

        at += part;
    }

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Commands                                                                    */
/* -------------------------------------------------------------------------- */

static void hci_event(const event_t* ev);

/*
 * Sends a command and waits for the controller's answer to it; other
 * events that arrive meanwhile are handled as usual.
 *
 * Returns the number of result bytes copied to `result` (the first is a
 * status, 0 = success; commands that only acknowledge return 0 bytes),
 * or -1 if it could not be sent, -2 if no answer came, -3 if refused
 * (result[0] is then the error code).
 */
static int hci_cmd(nq_u32 opcode, const nq_u8* params, nq_u32 param_len,
                   nq_u8* result, nq_u32 result_max, nq_u32 timeout_ms) {
    static nq_u8 packet[3 + 255];
    static event_t ev;
    nq_u8 lo = (nq_u8)(opcode & 0xFFu);
    nq_u8 hi = (nq_u8)(opcode >> 8);

    if (!g_present || param_len > 255u) {
        return -1;
    }

    packet[0] = lo;
    packet[1] = hi;
    packet[2] = (nq_u8)param_len;

    for (nq_u32 i = 0; i < param_len; i++) {
        packet[3 + i] = params[i];
    }

    process64_preempt_disable();

    /* class request to the interface: "here is a command" */
    int sent = g_present ? xhci64_control(g_hc, g_slot, 0x20u, 0x00u, 0, g_intf.number,
                                          packet, (nq_u16)(3u + param_len))
                         : -1;

    process64_preempt_enable();

    if (sent < 0) {
        return -1;
    }

    for (nq_u32 waited = 0; waited < timeout_ms; waited += 2u) {
        while (event_take(&ev)) {
            if (ev.data[0] == EV_COMMAND_COMPLETE && ev.length >= 5u &&
                ev.data[3] == lo && ev.data[4] == hi) {
                nq_u32 n = ev.length - 5u;

                if (n > result_max) {
                    n = result_max;
                }

                for (nq_u32 i = 0; i < n; i++) {
                    result[i] = ev.data[5u + i];
                }

                return (int)n;
            }

            if (ev.data[0] == EV_COMMAND_STATUS && ev.length >= 6u &&
                ev.data[4] == lo && ev.data[5] == hi) {
                if (ev.data[2] == 0) {
                    return 0;       /* accepted; the outcome comes as an event later */
                }

                if (result_max) {
                    result[0] = ev.data[2];
                }

                return -3;
            }

            hci_event(&ev);
        }

        sleep_ms(2);
    }

    return -2;
}

/* A command whose only interesting result is whether it worked. */
static int hci_do(const char* what, nq_u32 opcode, const nq_u8* params, nq_u32 param_len) {
    nq_u8 result[8];
    int n = hci_cmd(opcode, params, param_len, result, sizeof(result), 2000);

    if (n == 0 || (n > 0 && result[0] == 0)) {
        return 1;
    }

    bt_trace(what);

    if (n == -1) {
        bt_trace(": could not be sent\n");
    } else if (n == -2) {
        bt_trace(": no answer\n");
    } else {
        bt_trace(": error 0x");
        bt_trace_hex(result[0], 2);
        bt_trace("\n");
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* The saved headset                                                           */
/* -------------------------------------------------------------------------- */

/* File: "NQBT1", 6 address bytes, 16 key bytes, 32 name bytes. */
#define KEY_FILE_SIZE (5u + 6u + 16u + 32u)

static void key_load(void) {
    nq_u8 data[KEY_FILE_SIZE];
    nq_u64 size = 0;

    process64_preempt_disable();

    int ok = vfs64_usb_mounted() &&
             vfs64_read_file(KEY_FILE, data, sizeof(data), &size) && size == KEY_FILE_SIZE;

    process64_preempt_enable();

    if (!ok || data[0] != 'N' || data[1] != 'Q' || data[2] != 'B' || data[3] != 'T' || data[4] != '1') {
        return;
    }

    copy_bytes(g_key_addr, data + 5, 6);
    copy_bytes(g_key, data + 11, 16);
    data[KEY_FILE_SIZE - 1u] = 0;
    copy_name(g_key_name, (const char*)data + 27, sizeof(g_key_name));
    g_have_key = 1;

    bt_trace("saved headset: ");
    bt_trace(g_key_name[0] ? g_key_name : "(no name)");
    bt_trace(" ");
    trace_addr(g_key_addr);
    bt_trace("\n");
}

static void key_save(void) {
    nq_u8 data[KEY_FILE_SIZE];

    for (nq_u32 i = 0; i < KEY_FILE_SIZE; i++) {
        data[i] = 0;
    }

    data[0] = 'N'; data[1] = 'Q'; data[2] = 'B'; data[3] = 'T'; data[4] = '1';
    copy_bytes(data + 5, g_key_addr, 6);
    copy_bytes(data + 11, g_key, 16);

    for (nq_u32 i = 0; g_key_name[i] && i < 31u; i++) {
        data[27u + i] = (nq_u8)g_key_name[i];
    }

    process64_preempt_disable();

    int ok = vfs64_usb_mounted() && !vfs64_write_locked(KEY_FILE) &&
             vfs64_write_file(KEY_FILE, data, KEY_FILE_SIZE);

    process64_preempt_enable();

    bt_trace(ok ? "pairing saved to the boot stick\n"
                : "pairing NOT saved (no writable boot stick): it lasts until shutdown\n");
}

/* -------------------------------------------------------------------------- */
/* Events                                                                      */
/* -------------------------------------------------------------------------- */

static found_t* found_entry(const nq_u8* addr) {
    for (nq_u32 i = 0; i < g_found_count; i++) {
        if (same_addr(g_found[i].addr, addr)) {
            return &g_found[i];
        }
    }

    if (g_found_count >= FOUND_MAX) {
        return 0;
    }

    found_t* f = &g_found[g_found_count++];

    copy_bytes(f->addr, addr, 6);
    f->cod[0] = f->cod[1] = f->cod[2] = 0;
    f->page_mode = 1;
    f->clock = 0;
    f->name[0] = 0;

    return f;
}

/* The device's name, if the extra data from a scan answer carries it. */
static void name_from_extra(found_t* f, const nq_u8* extra, nq_u32 length) {
    for (nq_u32 i = 0; i + 1u < length; ) {
        nq_u32 len = extra[i];

        if (len == 0 || i + 1u + len > length) {
            break;
        }

        /* 0x09 complete name, 0x08 shortened name */
        if (extra[i + 1u] == 0x09u || (extra[i + 1u] == 0x08u && !f->name[0])) {
            nq_u32 n = len - 1u;
            char text[32];

            if (n > 31u) {
                n = 31u;
            }

            for (nq_u32 k = 0; k < n; k++) {
                text[k] = (char)extra[i + 2u + k];
            }

            text[n] = 0;
            copy_name(f->name, text, sizeof(f->name));
        }

        i += 1u + len;
    }
}

static void link_lost(void) {
    if (g_link != LINK_NONE) {
        g_link = LINK_NONE;
        a2dp_link_down();
    }

    g_frame_len = 0;
    g_frame_want = 0;
    g_auth_asked = 0;
    g_acl_free = g_acl_slots;
}

static void hci_event(const event_t* ev) {
    const nq_u8* p = ev->data + 2;
    nq_u32 len = ev->length - 2u;

    switch (ev->data[0]) {
        case EV_INQUIRY_RESULT:
        case EV_INQUIRY_RESULT_RSSI: {
            nq_u32 each = ev->data[0] == EV_INQUIRY_RESULT ? 14u : 14u;
            nq_u32 count = len ? p[0] : 0;

            /* plain: addr 6, mode 1, 2 reserved, class 3, clock 2
               with strength: addr 6, mode 1, 1 reserved, class 3, clock 2, strength 1 */
            for (nq_u32 i = 0; i < count && 1u + (i + 1u) * each <= len; i++) {
                const nq_u8* r = p + 1u + i * each;
                found_t* f = found_entry(r);
                nq_u32 skip = ev->data[0] == EV_INQUIRY_RESULT ? 9u : 8u;

                if (f) {
                    f->page_mode = r[6];
                    copy_bytes(f->cod, r + skip, 3);
                    f->clock = (nq_u16)(r[skip + 3u] | ((nq_u16)r[skip + 4u] << 8));
                }
            }

            break;
        }

        case EV_INQUIRY_RESULT_EXT:
            /* count 1, addr 6, mode 1, reserved 1, class 3, clock 2, strength 1, extra 240 */
            if (len >= 15u) {
                found_t* f = found_entry(p + 1);

                if (f) {
                    f->page_mode = p[7];
                    copy_bytes(f->cod, p + 9, 3);
                    f->clock = (nq_u16)(p[12] | ((nq_u16)p[13] << 8));
                    name_from_extra(f, p + 15, len - 15u);
                }
            }

            break;

        case EV_INQUIRY_COMPLETE:
            g_scanning = 0;
            break;

        case EV_REMOTE_NAME:
            /* status 1, addr 6, name 248 */
            if (len >= 8u) {
                if (p[0] == 0) {
                    found_t* f = found_entry(p + 1);
                    char text[32];
                    nq_u32 n = 0;

                    for (; n < 31u && 7u + n < len && p[7u + n]; n++) {
                        text[n] = (char)p[7u + n];
                    }

                    text[n] = 0;

                    if (f) {
                        copy_name(f->name, text, sizeof(f->name));
                    }

                    if (g_link != LINK_NONE && same_addr(p + 1, g_peer)) {
                        copy_name(g_peer_name, text, sizeof(g_peer_name));
                    }
                }

                if (same_addr(p + 1, g_name_addr)) {
                    g_name_pending = 0;
                }
            }

            break;

        case EV_CONNECTION_REQUEST:
            /* addr 6, class 3, link type 1 (1 = data link) */
            if (len >= 10u) {
                bt_trace("incoming call from ");
                trace_addr(p);
                bt_trace("\n");

                if (p[9] == 0x01u && g_link == LINK_NONE) {
                    copy_bytes(g_ask_addr, p, 6);
                    g_do_accept = 1;
                }
            }

            break;

        case EV_CONNECTION_COMPLETE:
            /* status 1, handle 2, addr 6, link type 1, encrypted 1 */
            if (len >= 11u) {
                if (p[0] != 0) {
                    bt_trace("connection failed, error 0x");
                    bt_trace_hex(p[0], 2);
                    bt_trace(p[0] == 0x04u ? " (the headset did not answer; is it on and in range?)\n" : "\n");

                    if (g_link == LINK_CALLING) {
                        g_link = LINK_NONE;
                    }

                    g_accepting = 0;
                    break;
                }

                g_handle = ((nq_u32)p[1] | ((nq_u32)p[2] << 8)) & 0x0FFFu;
                copy_bytes(g_peer, p + 3, 6);
                g_we_called = (g_link == LINK_CALLING) && !g_accepting;
                g_accepting = 0;
                g_link = LINK_UP;
                g_link_since = bt_now_ms();
                g_auth_asked = 0;
                g_acl_free = g_acl_slots;
                g_frame_len = 0;
                g_frame_want = 0;

                bt_trace("connected to ");
                trace_addr(g_peer);
                bt_trace("\n");

                if (g_we_called) {
                    g_do_auth = 1;
                }
            }

            break;

        case EV_DISCONNECTED:
            /* status 1, handle 2, reason 1 */
            if (len >= 4u && (((nq_u32)p[1] | ((nq_u32)p[2] << 8)) & 0x0FFFu) == g_handle &&
                g_link != LINK_NONE && g_link != LINK_CALLING) {
                bt_trace("disconnected, reason 0x");
                bt_trace_hex(p[3], 2);
                bt_trace("\n");
                link_lost();
            }

            break;

        case EV_LINK_KEY_REQUEST:
            if (len >= 6u) {
                copy_bytes(g_ask_addr, p, 6);
                g_do_key_reply = 1;
            }

            break;

        case EV_IO_CAPABILITY_REQUEST:
            if (len >= 6u) {
                copy_bytes(g_ask_addr, p, 6);
                g_do_io_reply = 1;
            }

            break;

        case EV_CONFIRM_REQUEST:
            if (len >= 6u) {
                copy_bytes(g_ask_addr, p, 6);
                g_do_confirm = 1;
            }

            break;

        case EV_PIN_REQUEST:
            if (len >= 6u) {
                copy_bytes(g_ask_addr, p, 6);
                g_do_pin = 1;
            }

            break;

        case EV_PAIRING_COMPLETE:
            if (len >= 1u) {
                bt_trace(p[0] == 0 ? "pairing done\n" : "pairing failed, error 0x");

                if (p[0] != 0) {
                    bt_trace_hex(p[0], 2);
                    bt_trace("\n");
                }
            }

            break;

        case EV_LINK_KEY:
            /* addr 6, key 16, type 1 */
            if (len >= 22u) {
                copy_bytes(g_key_addr, p, 6);
                copy_bytes(g_key, p + 6, 16);
                copy_name(g_key_name, g_peer_name, sizeof(g_key_name));
                g_have_key = 1;
                g_do_save = 1;
                bt_trace("received a link key\n");
            }

            break;

        case EV_AUTH_COMPLETE:
            /* status 1, handle 2 */
            if (len >= 3u) {
                if (p[0] == 0) {
                    bt_trace("authenticated\n");
                    g_do_encrypt = 1;
                } else {
                    bt_trace("authentication failed, error 0x");
                    bt_trace_hex(p[0], 2);

                    /* 0x05 wrong key, 0x06 key missing on the headset */
                    if ((p[0] == 0x05u || p[0] == 0x06u) && g_have_key) {
                        bt_trace(": the saved pairing no longer matches; forgotten.\n"
                                 "Put the headset in pairing mode and connect again.\n");
                        g_have_key = 0;
                    } else {
                        bt_trace("\n");
                    }
                }
            }

            break;

        case EV_ENCRYPTION_CHANGE:
            /* status 1, handle 2, on 1 */
            if (len >= 4u && p[0] == 0 && p[3] != 0 && g_link == LINK_UP) {
                bt_trace("link encrypted\n");
                g_link = LINK_SECURE;
                a2dp_link_up(g_we_called);
            } else if (len >= 1u && p[0] != 0) {
                bt_trace("encryption failed, error 0x");
                bt_trace_hex(p[0], 2);
                bt_trace("\n");
            }

            break;

        case EV_PACKETS_DONE:
            /* count 1, then handle 2 + packets 2 for each */
            if (len >= 1u) {
                for (nq_u32 i = 0; i < p[0] && 1u + (i + 1u) * 4u <= len; i++) {
                    g_acl_free += (nq_u32)p[1u + i * 4u + 2u] | ((nq_u32)p[1u + i * 4u + 3u] << 8);
                }

                if (g_acl_free > g_acl_slots) {
                    g_acl_free = g_acl_slots;
                }
            }

            break;

        default:
            break;
    }
}

/* A data packet from the headset: pieces of an L2CAP frame. */
static void acl_input(const packet_t* pkt) {
    nq_u32 head = (nq_u32)pkt->data[0] | ((nq_u32)pkt->data[1] << 8);
    nq_u32 boundary = (head >> 12) & 3u;
    const nq_u8* data = pkt->data + 4;
    nq_u32 length = pkt->length - 4u;

    if ((head & 0x0FFFu) != g_handle || g_link < LINK_UP) {
        return;
    }

    if (boundary == 2u || boundary == 0u) {
        /* start of a frame: its first two bytes give the payload length */
        g_frame_len = 0;
        g_frame_want = length >= 2u ? 4u + ((nq_u32)data[0] | ((nq_u32)data[1] << 8)) : 0;
    }

    if (!g_frame_want || g_frame_want > ACL_MAX || g_frame_len + length > ACL_MAX) {
        g_frame_len = 0;
        g_frame_want = 0;
        return;
    }

    copy_bytes(g_frame + g_frame_len, data, length);
    g_frame_len += length;

    if (g_frame_len >= g_frame_want) {
        a2dp_input(g_frame, g_frame_want);
        g_frame_len = 0;
        g_frame_want = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Things events asked for                                                     */
/* -------------------------------------------------------------------------- */

static void service(void) {
    nq_u8 params[24];

    if (g_do_accept) {
        g_do_accept = 0;
        copy_bytes(params, g_ask_addr, 6);
        params[6] = 0x01;       /* stay the answering side */
        g_link = LINK_CALLING;
        g_accepting = 1;

        if (!hci_do("accept", HCI_ACCEPT_CONNECTION, params, 7)) {
            g_link = LINK_NONE;
            g_accepting = 0;
        }
    }

    if (g_do_key_reply) {
        g_do_key_reply = 0;
        copy_bytes(params, g_ask_addr, 6);

        if (g_have_key && same_addr(g_ask_addr, g_key_addr)) {
            copy_bytes(params + 6, g_key, 16);
            bt_trace("using the saved pairing\n");
            hci_do("link key reply", HCI_LINK_KEY_REPLY, params, 22);
        } else {
            bt_trace("no saved pairing for this device: pairing afresh\n");
            hci_do("link key negative reply", HCI_LINK_KEY_NEGATIVE, params, 6);
        }
    }

    if (g_do_io_reply) {
        g_do_io_reply = 0;
        copy_bytes(params, g_ask_addr, 6);
        params[6] = 0x03;       /* no keyboard, no display */
        params[7] = 0x00;       /* no out-of-band data */
        params[8] = 0x04;       /* keep the pairing; no man-in-the-middle check */
        hci_do("pairing abilities reply", HCI_IO_CAPABILITY_REPLY, params, 9);
    }

    if (g_do_confirm) {
        g_do_confirm = 0;
        copy_bytes(params, g_ask_addr, 6);
        hci_do("pairing confirm", HCI_CONFIRM_REPLY, params, 6);
    }

    if (g_do_pin) {
        g_do_pin = 0;
        copy_bytes(params, g_ask_addr, 6);
        params[6] = 4;

        for (nq_u32 i = 0; i < 16u; i++) {
            params[7u + i] = i < 4u ? '0' : 0;
        }

        bt_trace("old-style pairing: trying PIN 0000\n");
        hci_do("PIN reply", HCI_PIN_REPLY, params, 23);
    }

    if (g_do_auth && g_link == LINK_UP) {
        g_do_auth = 0;
        g_auth_asked = 1;
        params[0] = (nq_u8)(g_handle & 0xFFu);
        params[1] = (nq_u8)(g_handle >> 8);
        hci_do("authenticate", HCI_AUTH_REQUESTED, params, 2);
    }

    if (g_do_encrypt && g_link == LINK_UP) {
        g_do_encrypt = 0;
        params[0] = (nq_u8)(g_handle & 0xFFu);
        params[1] = (nq_u8)(g_handle >> 8);
        params[2] = 1;
        hci_do("encrypt", HCI_SET_ENCRYPTION, params, 3);
    }

    if (g_do_save) {
        g_do_save = 0;
        key_save();
    }

    /* a headset that called us and did not start securing the link itself */
    if (g_link == LINK_UP && !g_auth_asked && bt_now_ms() - g_link_since >= 1500u) {
        g_do_auth = 1;
    }
}

/* Handles whatever has arrived. */
static void pump(void) {
    static event_t ev;
    static packet_t pkt;

    while (event_take(&ev)) {
        hci_event(&ev);
    }

    while (packet_take(&pkt)) {
        acl_input(&pkt);
    }

    service();
}

static void wait_ms(nq_u32 ms) {
    for (nq_u32 waited = 0; waited < ms; waited += 2u) {
        pump();
        sleep_ms(2);
    }
}

/* -------------------------------------------------------------------------- */
/* Setting the controller up                                                   */
/* -------------------------------------------------------------------------- */

static nq_u32 le32(const nq_u8* p) {
    return (nq_u32)p[0] | ((nq_u32)p[1] << 8) | ((nq_u32)p[2] << 16) | ((nq_u32)p[3] << 24);
}

static nq_u32 intel_name_part(nq_u32 top) {
    nq_u32 type = top & 0x00000FFFu;
    nq_u32 step = (top >> 24) & 0xFu;
    nq_u32 packed = ((type << 4) | step) & 0xFFFFu;

    return ((packed & 0xFFu) << 8) | (packed >> 8);
}

static void name_hex4(char* out, nq_u32 value) {
    for (nq_u32 i = 0; i < 4u; i++) {
        out[i] = "0123456789abcdef"[(value >> (12u - 4u * i)) & 0xFu];
    }
}

/*
 * Intel controllers: is the firmware running? Returns 1 if it is, 0 if
 * the chip is still waiting for a firmware file.
 */
static int intel_running(void) {
    static nq_u8 answer[255];
    nq_u8 ask = 0xFFu;
    int n = hci_cmd(HCI_INTEL_READ_VERSION, &ask, 1, answer, sizeof(answer), 1500);

    if (n < 0) {
        bt_trace("intel: no version answer; carrying on\n");
        return 1;
    }

    /* older chips: ten fixed bytes, the fifth says bootloader (6) or running (0x23) */
    if (n == 10 && answer[1] == 0x37u) {
        return answer[4] != 0x06u;
    }

    nq_u32 cnvi = 0;
    nq_u32 cnvr = 0;
    nq_u32 image = 3;

    for (nq_u32 i = 1; i + 2u <= (nq_u32)n; ) {
        nq_u32 type = answer[i];
        nq_u32 length = answer[i + 1u];

        if (i + 2u + length > (nq_u32)n) {
            break;
        }

        if (type == 0x10u && length >= 4u) cnvi = le32(&answer[i + 2u]);
        if (type == 0x11u && length >= 4u) cnvr = le32(&answer[i + 2u]);
        if (type == 0x1Cu && length >= 1u) image = answer[i + 2u];

        i += 2u + length;
    }

    /* "ibt-XXXX-YYYY.sfi" */
    g_firmware[0] = 'i'; g_firmware[1] = 'b'; g_firmware[2] = 't'; g_firmware[3] = '-';
    name_hex4(g_firmware + 4, intel_name_part(cnvi));
    g_firmware[8] = '-';
    name_hex4(g_firmware + 9, intel_name_part(cnvr));
    g_firmware[13] = '.'; g_firmware[14] = 's'; g_firmware[15] = 'f'; g_firmware[16] = 'i';
    g_firmware[17] = 0;

    return image == 0x03u;
}

static void controller_setup(void) {
    static nq_u8 params[248];
    nq_u8 result[16];

    bt_trace("controller: USB ");
    bt_trace_hex(g_vendor, 4);
    bt_trace(":");
    bt_trace_hex(g_product, 4);
    bt_trace("\n");

    if (g_vendor == USB_VENDOR_INTEL && !intel_running()) {
        bt_trace("controller: waiting for firmware (");
        bt_trace(g_firmware);
        bt_trace("), which this system cannot load yet.\n"
                 "Start Windows, then choose Restart (not Shut down) and boot NAQ OS:\n"
                 "the firmware Windows loaded stays in the chip.\n");
        g_ctrl = CTRL_NO_FIRMWARE;
        return;
    }

    g_ctrl = CTRL_FAILED;

    if (hci_cmd(HCI_RESET, 0, 0, result, sizeof(result), 4000) < 1 || result[0] != 0) {
        bt_trace("controller: reset failed\n");
        return;
    }

    wait_ms(100);

    /* How large a data packet may be, and how many the controller holds. */
    int n = hci_cmd(HCI_READ_BUFFER_SIZE, 0, 0, result, sizeof(result), 2000);

    if (n < 8 || result[0] != 0) {
        bt_trace("controller: could not read its buffer size\n");
        return;
    }

    g_acl_mtu = (nq_u32)result[1] | ((nq_u32)result[2] << 8);
    g_acl_slots = (nq_u32)result[4] | ((nq_u32)result[5] << 8);
    g_acl_free = g_acl_slots;

    if (g_acl_mtu > 1000u) {
        g_acl_mtu = 1000u;
    }

    n = hci_cmd(HCI_READ_BD_ADDR, 0, 0, result, sizeof(result), 2000);

    if (n >= 7 && result[0] == 0) {
        copy_bytes(g_own_addr, result + 1, 6);
    }

    /* Which events we want to hear about (the usual full set). */
    static const nq_u8 mask[8] = { 0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D };

    hci_do("event mask", HCI_SET_EVENT_MASK, mask, 8);

    params[0] = 1;
    hci_do("simple pairing", HCI_WRITE_SSP_MODE, params, 1);

    for (nq_u32 i = 0; i < 248u; i++) {
        params[i] = 0;
    }

    copy_bytes(params, (const nq_u8*)"NAQ OS", 6);
    hci_do("name", HCI_WRITE_LOCAL_NAME, params, 248);

    /* class of device: a computer */
    params[0] = 0x04;
    params[1] = 0x01;
    params[2] = 0x00;
    hci_do("class", HCI_WRITE_CLASS, params, 3);

    params[0] = 2;          /* scan answers with names where possible */
    hci_do("inquiry mode", HCI_WRITE_INQUIRY_MODE, params, 1);

    params[0] = 0x00;       /* give up calling after 5 s */
    params[1] = 0x20;
    hci_do("page timeout", HCI_WRITE_PAGE_TIMEOUT, params, 2);

    params[0] = 0x05;       /* allow role switch and sniff mode */
    params[1] = 0x00;
    hci_do("link policy", HCI_WRITE_LINK_POLICY, params, 2);

    params[0] = 0x02;       /* answer calls (so a paired headset can reconnect) */
    hci_do("scan enable", HCI_WRITE_SCAN_ENABLE, params, 1);

    g_ctrl = CTRL_READY;

    bt_trace("controller ready, address ");
    trace_addr(g_own_addr);
    bt_trace(", packets ");
    bt_trace_dec(g_acl_slots);
    bt_trace(" x ");
    bt_trace_dec(g_acl_mtu);
    bt_trace("\n");
}

/* -------------------------------------------------------------------------- */
/* Requests                                                                    */
/* -------------------------------------------------------------------------- */

static void do_scan(void) {
    nq_u8 params[10];

    g_found_count = 0;

    /* general inquiry, 8 * 1.28 s, any number of answers */
    params[0] = 0x33;
    params[1] = 0x8B;
    params[2] = 0x9E;
    params[3] = 8;
    params[4] = 0;

    g_scanning = 1;

    if (!hci_do("scan", HCI_INQUIRY, params, 5)) {
        g_scanning = 0;
        return;
    }

    for (nq_u32 waited = 0; g_scanning && waited < 14000u; waited += 20u) {
        wait_ms(20);
    }

    if (g_scanning) {
        hci_do("scan cancel", HCI_INQUIRY_CANCEL, 0, 0);
        g_scanning = 0;
    }

    /* Ask the ones that gave no name (at most a few; each can take seconds). */
    nq_u32 asked = 0;

    for (nq_u32 i = 0; i < g_found_count && asked < 5u; i++) {
        found_t* f = &g_found[i];

        if (f->name[0]) {
            continue;
        }

        copy_bytes(params, f->addr, 6);
        params[6] = f->page_mode;
        params[7] = 0;
        params[8] = (nq_u8)(f->clock & 0xFFu);
        params[9] = (nq_u8)((f->clock >> 8) | 0x80u);

        copy_bytes(g_name_addr, f->addr, 6);
        g_name_pending = 1;
        asked++;

        if (!hci_do("name request", HCI_REMOTE_NAME, params, 10)) {
            g_name_pending = 0;
            continue;
        }

        for (nq_u32 waited = 0; g_name_pending && waited < 6000u; waited += 20u) {
            wait_ms(20);
        }
    }

    bt_trace("scan found ");
    bt_trace_dec(g_found_count);
    bt_trace(g_found_count == 1u ? " device\n" : " devices\n");
}

static void do_connect(void) {
    nq_u8 params[13];
    found_t* f = 0;

    if (g_link != LINK_NONE) {
        bt_trace("already connected or connecting\n");
        return;
    }

    for (nq_u32 i = 0; i < g_found_count; i++) {
        if (same_addr(g_found[i].addr, g_request_addr)) {
            f = &g_found[i];
        }
    }

    g_peer_name[0] = 0;

    if (f) {
        copy_name(g_peer_name, f->name, sizeof(g_peer_name));
    } else if (g_have_key && same_addr(g_key_addr, g_request_addr)) {
        copy_name(g_peer_name, g_key_name, sizeof(g_peer_name));
    }

    bt_trace("calling ");
    trace_addr(g_request_addr);
    bt_trace("\n");

    copy_bytes(params, g_request_addr, 6);
    params[6] = 0x18;       /* packet types: all the usual ones */
    params[7] = 0xCC;
    params[8] = f ? f->page_mode : 0x01;
    params[9] = 0;
    params[10] = f ? (nq_u8)(f->clock & 0xFFu) : 0;
    params[11] = f ? (nq_u8)((f->clock >> 8) | 0x80u) : 0;
    params[12] = 0x01;      /* the headset may ask to swap roles */

    g_link = LINK_CALLING;
    g_we_called = 1;

    if (!hci_do("call", HCI_CREATE_CONNECTION, params, 13)) {
        g_link = LINK_NONE;
        return;
    }

    /* the connection event (or its failure) ends the wait */
    for (nq_u32 waited = 0; g_link == LINK_CALLING && waited < 12000u; waited += 20u) {
        wait_ms(20);
    }

    if (g_link == LINK_CALLING) {
        bt_trace("no answer to the call\n");
        g_link = LINK_NONE;
        return;
    }

    /* then pairing, encryption and the audio set-up run from events */
    for (nq_u32 waited = 0; g_link != LINK_NONE && !a2dp_streaming() && waited < 25000u; waited += 20u) {
        wait_ms(20);
        a2dp_tick();
    }
}

static void do_disconnect(void) {
    nq_u8 params[3];

    if (g_link < LINK_UP) {
        return;
    }

    params[0] = (nq_u8)(g_handle & 0xFFu);
    params[1] = (nq_u8)(g_handle >> 8);
    params[2] = 0x13;       /* "the user ended the connection" */
    hci_do("disconnect", HCI_DISCONNECT, params, 3);

    for (nq_u32 waited = 0; g_link != LINK_NONE && waited < 3000u; waited += 20u) {
        wait_ms(20);
    }
}

static void bt_task(void* arg) {
    (void)arg;

    int tried_saved = 0;

    for (;;) {
        if (g_ctrl == CTRL_ABSENT) {
            g_request = REQ_NONE;
            sleep_ms(200);
            continue;
        }

        if (g_ctrl == CTRL_NEW) {
            /* let the USB side settle, and the boot stick get mounted */
            sleep_ms(1500);
            key_load();
            controller_setup();
            continue;
        }

        if (g_ctrl != CTRL_READY) {
            g_request = REQ_NONE;
            sleep_ms(200);
            continue;
        }

        pump();
        a2dp_tick();

        /* once after start-up: call the headset paired last time, if it is on */
        if (!tried_saved && g_have_key && g_link == LINK_NONE && g_request == REQ_NONE) {
            tried_saved = 1;
            copy_bytes(g_request_addr, g_key_addr, 6);
            g_request = REQ_CONNECT;
        }

        tried_saved = 1;

        int request = g_request;

        if (request != REQ_NONE) {
            g_busy = 1;
            g_request = REQ_NONE;

            if (request == REQ_SCAN) {
                do_scan();
            } else if (request == REQ_CONNECT) {
                do_connect();
            } else if (request == REQ_DISCONNECT) {
                do_disconnect();
            } else if (request == REQ_FORGET) {
                do_disconnect();
                g_have_key = 0;

                for (nq_u32 i = 0; i < 16u; i++) {
                    g_key[i] = 0;
                }

                process64_preempt_disable();

                if (vfs64_usb_mounted() && !vfs64_write_locked(KEY_FILE)) {
                    vfs64_write_file(KEY_FILE, "none", 4);
                }

                process64_preempt_enable();
                bt_trace("saved pairing forgotten\n");
            }

            g_busy = 0;
        }

        sleep_ms(a2dp_streaming() ? 1 : 2);
    }
}

void bt64_init(void) {
    if (g_task_started) {
        return;
    }

    g_task_started = 1;

    if (process64_create_kernel_task_ex("bt", bt_task, 0, 0) <= 0) {
        k64_print("BT: could not start the Bluetooth task\n");
    }
}

/* -------------------------------------------------------------------------- */
/* Shell                                                                       */
/* -------------------------------------------------------------------------- */

static void print_addr(const nq_u8* addr) {
    char text[3];

    for (int i = 5; i >= 0; i--) {
        text[0] = "0123456789abcdef"[addr[i] >> 4];
        text[1] = "0123456789abcdef"[addr[i] & 0xFu];
        text[2] = 0;
        k64_print(text);

        if (i) {
            k64_print(":");
        }
    }
}

/* Prints what has been added to the trace since `*mark`. */
static void print_new_trace(nq_u64* mark) {
    static char copy[LOG_SIZE];
    nq_u32 n = 0;

    process64_preempt_disable();

    nq_u64 fresh = g_log_total - *mark;

    if (fresh > g_log_len) {
        fresh = g_log_len;
    }

    for (nq_u32 i = g_log_len - (nq_u32)fresh; i < g_log_len; i++) {
        copy[n++] = g_log[i];
    }

    copy[n] = 0;
    *mark = g_log_total;

    process64_preempt_enable();

    if (n) {
        k64_print(copy);
    }
}

/* Hands a request to the task and shows its trace until it is done. */
static void run_request(int request, nq_u32 limit_ms) {
    nq_u64 mark = g_log_total;

    if (g_busy || g_request != REQ_NONE) {
        k64_print("Bluetooth is busy with the previous command.\n");
        return;
    }

    g_request = request;

    for (nq_u32 waited = 0; waited < limit_ms; waited += 100u) {
        sleep_ms(100);
        print_new_trace(&mark);

        if (!g_busy && g_request == REQ_NONE && waited >= 300u) {
            break;
        }
    }

    print_new_trace(&mark);
}

static int is_audio(const found_t* f) {
    return (f->cod[1] & 0x1Fu) == 0x04u;    /* major class: audio/video */
}

static void print_found(void) {
    if (!g_found_count) {
        k64_print("Nothing was found. Is the headset in pairing mode (light blinking fast)?\n");
        return;
    }

    for (nq_u32 i = 0; i < g_found_count; i++) {
        const found_t* f = &g_found[i];

        k64_print("  ");
        k64_print_dec64(i + 1u);
        k64_print(". ");
        print_addr(f->addr);
        k64_print("  ");
        k64_print(f->name[0] ? f->name : "(no name)");
        k64_print(is_audio(f) ? "  [audio]\n" : "\n");
    }
}

static int word_is(const char* p, const char* word) {
    while (*word) {
        if (*p++ != *word++) {
            return 0;
        }
    }

    return *p == 0 || *p == ' ';
}

static int name_has(const char* name, const char* part) {
    for (; *name; name++) {
        nq_u32 i = 0;

        while (part[i] && name[i] == part[i]) {
            i++;
        }

        if (!part[i]) {
            return 1;
        }
    }

    return 0;
}

static void print_status(void) {
    if (g_ctrl == CTRL_ABSENT) {
        k64_print("No Bluetooth controller was found on USB.\n");
        return;
    }

    k64_print("Controller: ");

    switch (g_ctrl) {
        case CTRL_NEW:         k64_print("starting up\n"); break;
        case CTRL_NO_FIRMWARE: k64_print("waiting for firmware (see \"bt log\")\n"); break;
        case CTRL_FAILED:      k64_print("set-up failed (see \"bt log\")\n"); break;
        default:
            k64_print("ready, address ");
            print_addr(g_own_addr);
            k64_print("\n");
            break;
    }

    if (g_have_key) {
        k64_print("Paired headset: ");
        k64_print(g_key_name[0] ? g_key_name : "(no name)");
        k64_print("  ");
        print_addr(g_key_addr);
        k64_print("\n");
    }

    k64_print("Link: ");

    if (g_link == LINK_NONE) {
        k64_print("not connected\n");
    } else {
        k64_print(g_link == LINK_CALLING ? "calling " : (g_link == LINK_UP ? "connected to " : "secure link to "));
        print_addr(g_peer);
        k64_print("\nAudio: ");
        k64_print(a2dp_state());

        if (a2dp_streaming()) {
            k64_print(", ");
            k64_print_dec64(a2dp_packets_sent());
            k64_print(" packets sent, ");
            k64_print_dec64(a2dp_packets_skipped());
            k64_print(" skipped, bitpool ");
            k64_print_dec64(a2dp_bitpool());
        }

        k64_print("\n");
    }

    if (g_dropped) {
        k64_print("Packets lost on the way in: ");
        k64_print_dec64(g_dropped);
        k64_print("\n");
    }

    k64_print("Commands: bt pair | bt scan | bt connect [n] | bt disconnect | bt forget | bt log\n");
}

void bt64_shell(const char* args) {
    while (*args == ' ') {
        args++;
    }

    if (word_is(args, "log")) {
        nq_u64 mark = 0;

        print_new_trace(&mark);
        return;
    }

    if (*args == 0 || g_ctrl != CTRL_READY) {
        print_status();
        return;
    }

    if (word_is(args, "scan")) {
        k64_print("Scanning for about 10 seconds...\n");
        run_request(REQ_SCAN, 50000);
        print_found();
        return;
    }

    if (word_is(args, "pair")) {
        /*
         * Scan, pick the headset, connect. The choice: a device with
         * "G435" in its name, else the only audio device seen.
         */
        k64_print("Put the headset in pairing mode. Scanning for about 10 seconds...\n");
        run_request(REQ_SCAN, 50000);
        print_found();

        int pick = -1;
        nq_u32 audio = 0;

        for (nq_u32 i = 0; i < g_found_count; i++) {
            if (name_has(g_found[i].name, "G435")) {
                pick = (int)i;
                break;
            }

            if (is_audio(&g_found[i])) {
                audio++;
                pick = (int)i;
            }
        }

        if (pick < 0 || (audio > 1u && !name_has(g_found[pick].name, "G435"))) {
            k64_print("Could not tell which one is the headset. Use: bt connect <number>\n");
            return;
        }

        k64_print("Connecting to number ");
        k64_print_dec64((nq_u64)pick + 1u);
        k64_print("...\n");

        copy_bytes(g_request_addr, g_found[pick].addr, 6);
        run_request(REQ_CONNECT, 45000);
        print_status();
        return;
    }

    if (word_is(args, "connect")) {
        const char* p = args + 7;
        nq_u32 number = 0;

        while (*p == ' ') {
            p++;
        }

        while (*p >= '0' && *p <= '9') {
            number = number * 10u + (nq_u32)(*p++ - '0');
        }

        if (number >= 1u && number <= g_found_count) {
            copy_bytes(g_request_addr, g_found[number - 1u].addr, 6);
        } else if (number == 0 && g_have_key) {
            copy_bytes(g_request_addr, g_key_addr, 6);
        } else {
            k64_print("Use \"bt scan\" first, then \"bt connect <number>\".\n");
            return;
        }

        run_request(REQ_CONNECT, 45000);
        print_status();
        return;
    }

    if (word_is(args, "disconnect")) {
        run_request(REQ_DISCONNECT, 5000);
        print_status();
        return;
    }

    if (word_is(args, "forget")) {
        run_request(REQ_FORGET, 6000);
        return;
    }

    print_status();
}
