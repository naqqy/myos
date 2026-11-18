#include "usb64.h"
#include "xhci64.h"
#include "usbhid64.h"
#include "usbmsc64.h"
#include "../bt/bt64.h"

#include "../k64_console.h"
#include "../arch/x86_64/timer64.h"
#include "../proc/process64.h"

/*
 * USB core. See usb64.h.
 *
 * The task loop does three things over and over:
 *   - lets the controllers deliver finished transfers (keyboard and mouse
 *     reports arrive this way),
 *   - looks at the controllers' own ports for plugged / unplugged devices,
 *   - asks every hub the same question about its ports.
 *
 * A new device is "enumerated": reset its port, give it an address, read
 * its descriptors, pick its configuration, then start whatever driver
 * fits (HID keyboard/mouse, or the hub code in this file).
 */

#define USB64_MAX_DEVICES   24u
#define USB64_MAX_HUB_PORTS 15u
#define USB64_MAX_TIERS     5u
#define USB64_MAX_HID       4u

#define USB64_LOG_SIZE 4096u

/*
 * How often to look at ports. Port-change events from the controller
 * trigger a root scan at once; this is the fallback.
 */
#define ROOT_SCAN_MS 250u
#define HUB_POLL_MS  250u

/*
 * A port whose device fails to enumerate is retried this many times,
 * then left alone until the device is unplugged.
 */
#define ATTACH_TRIES 3u

/*
 * Start-up is not called finished before this long has passed, however
 * quiet the ports look: a USB 3 stick needs time to train its link
 * before the controller reports it as connected.
 */
#define BOOT_SETTLE_MS 800u

#define KIND_KEYBOARD 0x1u
#define KIND_POINTER  0x2u
#define KIND_HUB      0x4u
#define KIND_STORAGE  0x8u
#define KIND_BLUETOOTH 0x10u

/*
 * Standard requests.
 */
#define REQ_GET_STATUS        0u
#define REQ_CLEAR_FEATURE     1u
#define REQ_SET_FEATURE       3u
#define REQ_GET_DESCRIPTOR    6u
#define REQ_SET_CONFIGURATION 9u
#define REQ_SET_HUB_DEPTH     12u

#define DESC_DEVICE    1u
#define DESC_CONFIG    2u
#define DESC_INTERFACE 4u
#define DESC_ENDPOINT  5u
#define DESC_HID       0x21u
#define DESC_HID_REPORT 0x22u
#define DESC_HUB       0x29u
#define DESC_HUB_SS    0x2Au

#define HUB_FEATURE_PORT_RESET   4u
#define HUB_FEATURE_PORT_POWER   8u
#define HUB_FEATURE_C_CONNECTION 16u
#define HUB_FEATURE_C_RESET      20u

typedef struct usb64_device {
    int used;

    nq_u8 hc_index;
    xhci64_t* hc;
    nq_u8 slot;

    nq_u8 speed;
    nq_u8 root_port;
    nq_u32 route;
    nq_u8 tier;         /* 0 = plugged straight into the controller */

    int parent;         /* device index of the hub above, -1 for none */
    nq_u8 parent_port;

    nq_u8 tt_slot;
    nq_u8 tt_port;

    nq_u16 vendor;
    nq_u16 product;
    nq_u32 kinds;

    /*
     * Hub state.
     */
    nq_u32 hub_ports;
    int hub_usb3;
    nq_u8 child[USB64_MAX_HUB_PORTS + 1u];   /* device index + 1, 0 = empty */
    nq_u8 tries[USB64_MAX_HUB_PORTS + 1u];
} usb64_device_t;

static usb64_device_t g_dev[USB64_MAX_DEVICES];

/*
 * What sits on each controller's own ports: device index + 1.
 */
static nq_u8 g_root_child[XHCI64_MAX_CONTROLLERS][XHCI64_MAX_PORTS + 1u];
static nq_u8 g_root_tries[XHCI64_MAX_CONTROLLERS][XHCI64_MAX_PORTS + 1u];

static nq_u8 g_cfg[1024];

static int g_started = 0;
static volatile int g_boot_done = 0;
static nq_u32 g_attached_this_pass = 0;

/*
 * ------------------------------------------------------------
 * Log
 * ------------------------------------------------------------
 */

static char g_log[USB64_LOG_SIZE];
static volatile nq_u32 g_log_len = 0;

static void log_char(char c) {
    if (g_log_len + 1u >= USB64_LOG_SIZE) {
        /*
         * Full: drop the older half.
         */
        nq_u32 half = USB64_LOG_SIZE / 2u;

        for (nq_u32 i = half; i < g_log_len; i++) {
            g_log[i - half] = g_log[i];
        }

        g_log_len -= half;
    }

    g_log[g_log_len] = c;
    g_log_len++;
    g_log[g_log_len] = 0;
}

void usb64_log(const char* s) {
    if (!s) {
        return;
    }

    while (*s) {
        log_char(*s++);
    }
}

void usb64_log_hex(nq_u64 value, nq_u32 digits) {
    static const char hex[] = "0123456789ABCDEF";

    if (digits == 0 || digits > 16u) {
        digits = 16u;
    }

    for (nq_u32 i = digits; i > 0; i--) {
        log_char(hex[(value >> ((i - 1u) * 4u)) & 0xFu]);
    }
}

void usb64_log_dec(nq_u64 value) {
    char buf[24];
    int n = 0;

    if (value == 0) {
        log_char('0');
        return;
    }

    while (value && n < (int)sizeof(buf)) {
        buf[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (n > 0) {
        log_char(buf[--n]);
    }
}

/*
 * ------------------------------------------------------------
 * Requests
 * ------------------------------------------------------------
 */

static int get_descriptor(usb64_device_t* d, nq_u8 type, nq_u8 index, void* out, nq_u16 length) {
    return xhci64_control(
        d->hc,
        d->slot,
        0x80u,
        REQ_GET_DESCRIPTOR,
        (nq_u16)(((nq_u16)type << 8) | index),
        0,
        out,
        length
    );
}

static int hub_port_feature(usb64_device_t* hub, nq_u32 port, nq_u16 feature, int set) {
    return xhci64_control(
        hub->hc,
        hub->slot,
        0x23u,
        set ? REQ_SET_FEATURE : REQ_CLEAR_FEATURE,
        feature,
        (nq_u16)port,
        0,
        0
    ) >= 0;
}

static int hub_port_status(usb64_device_t* hub, nq_u32 port, nq_u32* out_status, nq_u32* out_change) {
    nq_u8 buf[4];

    if (xhci64_control(hub->hc, hub->slot, 0xA3u, REQ_GET_STATUS, 0, (nq_u16)port, buf, 4) != 4) {
        return 0;
    }

    *out_status = (nq_u32)buf[0] | ((nq_u32)buf[1] << 8);
    *out_change = (nq_u32)buf[2] | ((nq_u32)buf[3] << 8);

    return 1;
}

/*
 * ------------------------------------------------------------
 * Attach / detach
 * ------------------------------------------------------------
 */

static int device_alloc(void) {
    for (nq_u32 i = 0; i < USB64_MAX_DEVICES; i++) {
        if (!g_dev[i].used) {
            nq_u8* p = (nq_u8*)&g_dev[i];

            for (nq_u32 n = 0; n < sizeof(g_dev[i]); n++) {
                p[n] = 0;
            }

            return (int)i;
        }
    }

    return -1;
}

static void log_place(const usb64_device_t* d) {
    usb64_log(" on port ");
    usb64_log_dec(d->root_port);

    for (nq_u32 t = 0; t < d->tier; t++) {
        usb64_log(".");
        usb64_log_dec((d->route >> (4u * t)) & 0xFu);
    }
}

static void log_kind(nq_u32 kinds) {
    if (kinds & KIND_HUB) {
        usb64_log("hub");
    } else if (kinds & KIND_STORAGE) {
        usb64_log("storage");
    } else if (kinds & KIND_BLUETOOTH) {
        usb64_log("Bluetooth");
    } else if ((kinds & KIND_KEYBOARD) && (kinds & KIND_POINTER)) {
        usb64_log("keyboard + mouse");
    } else if (kinds & KIND_KEYBOARD) {
        usb64_log("keyboard");
    } else if (kinds & KIND_POINTER) {
        usb64_log("mouse");
    } else {
        usb64_log("device (no driver)");
    }
}

static void device_detach(int index) {
    if (index < 0 || index >= (int)USB64_MAX_DEVICES || !g_dev[index].used) {
        return;
    }

    usb64_device_t* d = &g_dev[index];

    /*
     * Everything behind a hub goes with it.
     */
    for (nq_u32 port = 1; port <= USB64_MAX_HUB_PORTS; port++) {
        if (d->child[port]) {
            device_detach((int)d->child[port] - 1);
            d->child[port] = 0;
        }
    }

    usb64_log("USB: ");
    log_kind(d->kinds);
    usb64_log(" removed");
    log_place(d);
    usb64_log("\n");

    usbmsc64_detach(d->hc, d->slot);
    usbhid64_detach(d->hc, d->slot);
    bt64_detach(d->hc, d->slot);
    xhci64_device_close(d->hc, d->slot);

    d->used = 0;
}

typedef struct parsed_config {
    nq_u8 value;
    int is_hub;

    nq_u32 hid_count;
    usbhid64_interface_t hid[USB64_MAX_HID];

    /*
     * Mass storage interface (class 8, SCSI commands, bulk-only).
     */
    int has_storage;
    usbmsc64_interface_t storage;

    /*
     * Bluetooth controller interface (class E0, subclass 1, protocol 1).
     */
    int has_bluetooth;
    bt64_interface_t bluetooth;
} parsed_config_t;

/*
 * Walks a configuration descriptor: a list of interface descriptors, each
 * followed by its class-specific and endpoint descriptors. A composite
 * device (a wireless receiver, say) has several interfaces; each HID one
 * is recorded with its first interrupt IN endpoint.
 */
static void parse_config(const nq_u8* cfg, nq_u32 total, parsed_config_t* out) {
    usbhid64_interface_t* cur = 0;
    int in_storage = 0;
    int in_bluetooth = 0;
    nq_u32 i = 0;

    out->value = cfg[5];
    out->is_hub = 0;
    out->hid_count = 0;
    out->has_storage = 0;
    out->storage.number = 0;
    out->storage.ep_in = 0;
    out->storage.ep_out = 0;
    out->storage.in_packet = 0;
    out->storage.out_packet = 0;
    out->has_bluetooth = 0;
    out->bluetooth.number = 0;
    out->bluetooth.ep_events = 0;
    out->bluetooth.ep_data_in = 0;
    out->bluetooth.ep_data_out = 0;
    out->bluetooth.events_packet = 0;
    out->bluetooth.data_in_packet = 0;
    out->bluetooth.data_out_packet = 0;
    out->bluetooth.events_interval = 0;

    while (i + 2u <= total) {
        nq_u32 length = cfg[i];
        nq_u32 type = cfg[i + 1u];

        if (length < 2u || i + length > total) {
            break;
        }

        if (type == DESC_INTERFACE && length >= 9u) {
            nq_u8 alternate = cfg[i + 3u];
            nq_u8 cls = cfg[i + 5u];

            cur = 0;
            in_storage = 0;
            in_bluetooth = 0;

            if (alternate == 0 && cls == 0xE0u && cfg[i + 6u] == 1u && cfg[i + 7u] == 1u &&
                !out->has_bluetooth) {
                out->has_bluetooth = 1;
                out->bluetooth.number = cfg[i + 2u];
                in_bluetooth = 1;
            } else if (alternate == 0 && cls == 8u && cfg[i + 6u] == 6u && cfg[i + 7u] == 0x50u &&
                !out->has_storage) {
                out->has_storage = 1;
                out->storage.number = cfg[i + 2u];
                in_storage = 1;
            } else if (alternate == 0 && cls == 3u && out->hid_count < USB64_MAX_HID) {
                cur = &out->hid[out->hid_count++];

                cur->number = cfg[i + 2u];
                cur->subclass = cfg[i + 6u];
                cur->protocol = cfg[i + 7u];
                cur->report_length = 0;
                cur->ep_address = 0;
                cur->ep_max_packet = 0;
                cur->ep_interval = 0;
            } else if (alternate == 0 && cls == 9u) {
                out->is_hub = 1;
            }
        } else if (type == DESC_HID && length >= 9u && cur) {
            nq_u32 count = cfg[i + 5u];

            for (nq_u32 n = 0; n < count && 6u + n * 3u + 3u <= length; n++) {
                const nq_u8* entry = &cfg[i + 6u + n * 3u];

                if (entry[0] == DESC_HID_REPORT) {
                    cur->report_length = (nq_u16)(entry[1] | ((nq_u16)entry[2] << 8));
                    break;
                }
            }
        } else if (type == DESC_ENDPOINT && length >= 7u && in_bluetooth) {
            nq_u8 address = cfg[i + 2u];
            nq_u8 kind = cfg[i + 3u] & 0x3u;
            nq_u16 packet = (nq_u16)(cfg[i + 4u] | ((nq_u16)cfg[i + 5u] << 8));

            if (kind == 3u && (address & 0x80u) && !out->bluetooth.ep_events) {
                out->bluetooth.ep_events = address;
                out->bluetooth.events_packet = packet;
                out->bluetooth.events_interval = cfg[i + 6u];
            } else if (kind == 2u && (address & 0x80u) && !out->bluetooth.ep_data_in) {
                out->bluetooth.ep_data_in = address;
                out->bluetooth.data_in_packet = packet;
            } else if (kind == 2u && !(address & 0x80u) && !out->bluetooth.ep_data_out) {
                out->bluetooth.ep_data_out = address;
                out->bluetooth.data_out_packet = packet;
            }
        } else if (type == DESC_ENDPOINT && length >= 7u && in_storage) {
            nq_u8 address = cfg[i + 2u];
            nq_u16 packet = (nq_u16)(cfg[i + 4u] | ((nq_u16)cfg[i + 5u] << 8));

            if ((cfg[i + 3u] & 0x3u) == 2u) {
                if ((address & 0x80u) && !out->storage.ep_in) {
                    out->storage.ep_in = address;
                    out->storage.in_packet = packet;
                } else if (!(address & 0x80u) && !out->storage.ep_out) {
                    out->storage.ep_out = address;
                    out->storage.out_packet = packet;
                }
            }
        } else if (type == DESC_ENDPOINT && length >= 7u && cur && !cur->ep_address) {
            nq_u8 address = cfg[i + 2u];
            nq_u8 attributes = cfg[i + 3u];

            if ((address & 0x80u) && (attributes & 0x3u) == 3u) {
                cur->ep_address = address;
                cur->ep_max_packet = (nq_u16)(cfg[i + 4u] | ((nq_u16)cfg[i + 5u] << 8));
                cur->ep_interval = cfg[i + 6u];
            }
        }

        i += length;
    }
}

static int hub_setup(usb64_device_t* d) {
    nq_u8 buf[16];

    d->hub_usb3 = d->speed >= XHCI64_SPEED_SUPER;

    int got = xhci64_control(
        d->hc,
        d->slot,
        0xA0u,
        REQ_GET_DESCRIPTOR,
        (nq_u16)((d->hub_usb3 ? DESC_HUB_SS : DESC_HUB) << 8),
        0,
        buf,
        d->hub_usb3 ? 12u : 9u
    );

    if (got < 7) {
        return 0;
    }

    nq_u32 ports = buf[2];
    nq_u32 characteristics = (nq_u32)buf[3] | ((nq_u32)buf[4] << 8);
    nq_u32 power_good_ms = (nq_u32)buf[5] * 2u;

    if (ports > USB64_MAX_HUB_PORTS) {
        ports = USB64_MAX_HUB_PORTS;
    }

    if (d->hub_usb3) {
        xhci64_control(d->hc, d->slot, 0x20u, REQ_SET_HUB_DEPTH, d->tier, 0, 0, 0);
    }

    if (!xhci64_configure(d->hc, d->slot, 0, 0, 1, ports, (characteristics >> 5) & 0x3u)) {
        return 0;
    }

    for (nq_u32 port = 1; port <= ports; port++) {
        hub_port_feature(d, port, HUB_FEATURE_PORT_POWER, 1);
    }

    xhci64_idle_ms(power_good_ms < 100u ? 100u : power_good_ms);

    d->hub_ports = ports;

    return 1;
}

/*
 * Enumerates the device that has just been reset on a root port
 * (parent < 0) or on a hub port. Returns its device index or -1.
 */
static int device_attach(nq_u32 hc_index, int parent, nq_u32 port, nq_u8 speed) {
    xhci64_t* hc = xhci64_get(hc_index);
    xhci64_device_info_t info;
    nq_u8 tier = 0;

    info.root_port = (nq_u8)port;
    info.route = 0;
    info.speed = speed;
    info.tt_slot = 0;
    info.tt_port = 0;

    if (parent >= 0) {
        usb64_device_t* hub = &g_dev[parent];

        if (hub->tier >= USB64_MAX_TIERS) {
            usb64_log("USB: too many hubs in a row\n");
            return -1;
        }

        info.root_port = hub->root_port;
        info.route = hub->route | ((port > 15u ? 15u : port) << (4u * hub->tier));
        tier = (nq_u8)(hub->tier + 1u);

        /*
         * A low/full-speed device behind a high-speed hub is reached
         * through that hub's transaction translator.
         */
        if (speed == XHCI64_SPEED_LOW || speed == XHCI64_SPEED_FULL) {
            if (hub->speed == XHCI64_SPEED_HIGH) {
                info.tt_slot = hub->slot;
                info.tt_port = (nq_u8)port;
            } else {
                info.tt_slot = hub->tt_slot;
                info.tt_port = hub->tt_port;
            }
        }
    }

    int index = device_alloc();

    if (index < 0) {
        usb64_log("USB: too many devices\n");
        return -1;
    }

    nq_u8 slot = xhci64_device_open(hc, &info);

    if (!slot) {
        usb64_log("USB: device did not accept an address\n");
        return -1;
    }

    usb64_device_t* d = &g_dev[index];

    d->used = 1;
    d->hc_index = (nq_u8)hc_index;
    d->hc = hc;
    d->slot = slot;
    d->speed = speed;
    d->root_port = info.root_port;
    d->route = info.route;
    d->tier = tier;
    d->parent = parent;
    d->parent_port = (nq_u8)port;
    d->tt_slot = info.tt_slot;
    d->tt_port = info.tt_port;

    nq_u8 desc[18];
    parsed_config_t cfg;
    int ok = 0;

    do {
        /*
         * Full-speed devices choose their own endpoint 0 packet size; it
         * is in the first 8 bytes of the device descriptor.
         */
        if (speed == XHCI64_SPEED_FULL) {
            if (get_descriptor(d, DESC_DEVICE, 0, desc, 8) < 8) {
                break;
            }

            nq_u32 packet = desc[7];

            if ((packet == 16u || packet == 32u || packet == 64u) &&
                !xhci64_set_ep0_max_packet(hc, slot, packet)) {
                break;
            }
        }

        if (get_descriptor(d, DESC_DEVICE, 0, desc, 18) < 18) {
            break;
        }

        d->vendor = (nq_u16)(desc[8] | ((nq_u16)desc[9] << 8));
        d->product = (nq_u16)(desc[10] | ((nq_u16)desc[11] << 8));

        if (get_descriptor(d, DESC_CONFIG, 0, g_cfg, 9) < 9) {
            break;
        }

        nq_u32 total = (nq_u32)g_cfg[2] | ((nq_u32)g_cfg[3] << 8);

        if (total > sizeof(g_cfg)) {
            total = sizeof(g_cfg);
        }

        int got = get_descriptor(d, DESC_CONFIG, 0, g_cfg, (nq_u16)total);

        if (got < 9) {
            break;
        }

        parse_config(g_cfg, (nq_u32)got, &cfg);

        /*
         * Endpoints first, then SET_CONFIGURATION, so the controller is
         * ready by the time the device starts answering on them.
         */
        xhci64_endpoint_t endpoints[USB64_MAX_HID + 2u + 3u];
        nq_u32 endpoint_count = 0;
        int storage_ok = cfg.has_storage && cfg.storage.ep_in && cfg.storage.ep_out;

        if (storage_ok) {
            endpoints[0].address = cfg.storage.ep_in;
            endpoints[0].attributes = 2u;
            endpoints[0].max_packet = cfg.storage.in_packet;
            endpoints[0].interval = 0;

            endpoints[1].address = cfg.storage.ep_out;
            endpoints[1].attributes = 2u;
            endpoints[1].max_packet = cfg.storage.out_packet;
            endpoints[1].interval = 0;

            endpoint_count = 2;
        }

        int bluetooth_ok = cfg.has_bluetooth && cfg.bluetooth.ep_events &&
                           cfg.bluetooth.ep_data_in && cfg.bluetooth.ep_data_out;

        if (bluetooth_ok) {
            endpoints[endpoint_count].address = cfg.bluetooth.ep_events;
            endpoints[endpoint_count].attributes = 3u;
            endpoints[endpoint_count].max_packet = cfg.bluetooth.events_packet;
            endpoints[endpoint_count].interval = cfg.bluetooth.events_interval;
            endpoint_count++;

            endpoints[endpoint_count].address = cfg.bluetooth.ep_data_in;
            endpoints[endpoint_count].attributes = 2u;
            endpoints[endpoint_count].max_packet = cfg.bluetooth.data_in_packet;
            endpoints[endpoint_count].interval = 0;
            endpoint_count++;

            endpoints[endpoint_count].address = cfg.bluetooth.ep_data_out;
            endpoints[endpoint_count].attributes = 2u;
            endpoints[endpoint_count].max_packet = cfg.bluetooth.data_out_packet;
            endpoints[endpoint_count].interval = 0;
            endpoint_count++;
        }

        for (nq_u32 i = 0; i < cfg.hid_count; i++) {
            if (!cfg.hid[i].ep_address) {
                continue;
            }

            endpoints[endpoint_count].address = cfg.hid[i].ep_address;
            endpoints[endpoint_count].attributes = 3u;
            endpoints[endpoint_count].max_packet = cfg.hid[i].ep_max_packet;
            endpoints[endpoint_count].interval = cfg.hid[i].ep_interval;
            endpoint_count++;
        }

        if (endpoint_count && !xhci64_configure(hc, slot, endpoints, endpoint_count, 0, 0, 0)) {
            break;
        }

        if (xhci64_control(hc, slot, 0x00u, REQ_SET_CONFIGURATION, cfg.value, 0, 0, 0) < 0) {
            break;
        }

        if (cfg.is_hub || desc[4] == 9u) {
            if (!hub_setup(d)) {
                break;
            }

            d->kinds |= KIND_HUB;
        }

        if (storage_ok) {
            /*
             * Shown in the listing even if the disk itself is unusable
             * (an empty card reader, say).
             */
            d->kinds |= KIND_STORAGE;
            usbmsc64_attach(hc, slot, &cfg.storage);
        }

        if (bluetooth_ok) {
            d->kinds |= KIND_BLUETOOTH;
            bt64_attach(hc, slot, &cfg.bluetooth, d->vendor, d->product);
        }

        for (nq_u32 i = 0; i < cfg.hid_count; i++) {
            if (!cfg.hid[i].ep_address) {
                continue;
            }

            int kind = usbhid64_attach(hc, slot, &cfg.hid[i]);

            if (kind == USBHID64_KEYBOARD) {
                d->kinds |= KIND_KEYBOARD;
            } else if (kind == USBHID64_POINTER) {
                d->kinds |= KIND_POINTER;
            }
        }

        ok = 1;
    } while (0);

    if (!ok) {
        usb64_log("USB: device");
        log_place(d);
        usb64_log(" did not answer correctly\n");

        usbmsc64_detach(hc, slot);
        usbhid64_detach(hc, slot);
        bt64_detach(hc, slot);
        xhci64_device_close(hc, slot);
        d->used = 0;

        return -1;
    }

    usb64_log("USB: ");
    log_kind(d->kinds);
    usb64_log(" ");
    usb64_log_hex(d->vendor, 4);
    usb64_log(":");
    usb64_log_hex(d->product, 4);
    log_place(d);
    usb64_log("\n");

    g_attached_this_pass++;

    return index;
}

/*
 * ------------------------------------------------------------
 * Port scanning
 * ------------------------------------------------------------
 */

/*
 * Looks at every port of one controller. Returns how many ports have a
 * device plugged in that is not set up yet but will be tried again.
 */
static nq_u32 root_scan(nq_u32 hc_index) {
    xhci64_t* hc = xhci64_get(hc_index);
    nq_u32 ports = xhci64_port_count(hc);
    nq_u32 pending = 0;

    for (nq_u32 port = 1; port <= ports; port++) {
        int changed = 0;
        int connected = xhci64_port_connected(hc, port, &changed);
        nq_u8* child = &g_root_child[hc_index][port];
        nq_u8* tries = &g_root_tries[hc_index][port];

        if (*child && (!connected || changed)) {
            device_detach((int)*child - 1);
            *child = 0;
        }

        if (!connected) {
            *tries = 0;
            continue;
        }

        if (*child || *tries >= ATTACH_TRIES) {
            continue;
        }

        /*
         * Let the plug settle before talking to the device.
         */
        xhci64_idle_ms(100);

        if (!xhci64_port_connected(hc, port, 0)) {
            continue;
        }

        nq_u8 speed = 0;
        int index = -1;

        if (xhci64_port_reset(hc, port, &speed)) {
            index = device_attach(hc_index, -1, port, speed);
        }

        if (index >= 0) {
            *child = (nq_u8)(index + 1);
            *tries = 0;
        } else {
            (*tries)++;

            if (*tries < ATTACH_TRIES) {
                pending++;
            }
        }
    }

    return pending;
}

static void hub_port_attach(int hub_index, nq_u32 port) {
    usb64_device_t* hub = &g_dev[hub_index];
    nq_u32 status = 0;
    nq_u32 change = 0;

    xhci64_idle_ms(100);

    if (!hub_port_feature(hub, port, HUB_FEATURE_PORT_RESET, 1)) {
        hub->tries[port]++;
        return;
    }

    int reset_done = 0;

    for (nq_u32 i = 0; i < 50u; i++) {
        xhci64_idle_ms(10);

        if (!hub_port_status(hub, port, &status, &change)) {
            break;
        }

        if (change & 0x10u) {
            reset_done = 1;
            break;
        }
    }

    hub_port_feature(hub, port, HUB_FEATURE_C_RESET, 0);
    hub_port_feature(hub, port, HUB_FEATURE_C_CONNECTION, 0);

    xhci64_idle_ms(20);

    if (!reset_done || !hub_port_status(hub, port, &status, &change) ||
        !(status & 0x1u) || !(status & 0x2u)) {
        hub->tries[port]++;
        return;
    }

    nq_u8 speed;

    if (hub->hub_usb3) {
        speed = XHCI64_SPEED_SUPER;
    } else if (status & 0x200u) {
        speed = XHCI64_SPEED_LOW;
    } else if (status & 0x400u) {
        speed = XHCI64_SPEED_HIGH;
    } else {
        speed = XHCI64_SPEED_FULL;
    }

    int index = device_attach(hub->hc_index, hub_index, port, speed);

    if (index >= 0) {
        hub->child[port] = (nq_u8)(index + 1);
        hub->tries[port] = 0;
    } else {
        hub->tries[port]++;
    }
}

static void hub_poll(int hub_index) {
    usb64_device_t* hub = &g_dev[hub_index];

    for (nq_u32 port = 1; port <= hub->hub_ports; port++) {
        nq_u32 status = 0;
        nq_u32 change = 0;

        if (!hub->used) {
            return;
        }

        if (!hub_port_status(hub, port, &status, &change)) {
            /*
             * The hub itself may be gone; the scan of the port above it
             * will notice.
             */
            return;
        }

        int connected = (status & 0x1u) != 0;
        int changed = (change & 0x1u) != 0;

        if (changed) {
            hub_port_feature(hub, port, HUB_FEATURE_C_CONNECTION, 0);
        }

        if (hub->child[port] && (!connected || changed)) {
            device_detach((int)hub->child[port] - 1);
            hub->child[port] = 0;
        }

        if (!connected) {
            hub->tries[port] = 0;
            continue;
        }

        if (hub->child[port] || hub->tries[port] >= ATTACH_TRIES) {
            continue;
        }

        hub_port_attach(hub_index, port);
    }
}

/*
 * ------------------------------------------------------------
 * Task
 * ------------------------------------------------------------
 */

static nq_u64 ms_now(void) {
    nq_u64 hz = timer64_get_frequency();

    if (!hz) {
        hz = 1000u;
    }

    return (timer64_get_ticks() * 1000u) / hz;
}

static void usb_task(void* arg) {
    (void)arg;

    /*
     * From here on this task holds the task-switch guard whenever it is
     * running; see the locking rule in xhci64.h.
     */
    xhci64_enter_task();

    nq_u32 controllers = xhci64_init_all();

    if (!controllers) {
        g_boot_done = 1;
        process64_preempt_enable();
        process64_exit();
        return;
    }

    /*
     * Freshly powered ports need a moment to notice what is plugged in.
     */
    xhci64_idle_ms(150);

    nq_u64 started = ms_now();
    nq_u64 last_root = 0;
    nq_u64 last_hub = 0;
    int first = 1;

    for (;;) {
        nq_u64 now = ms_now();
        /*
         * While starting up, look at the ports on every pass: real devices
         * often need a second try, and USB 3 devices take a moment to show
         * up at all.
         */
        int scan_root = first || !g_boot_done || (now - last_root) >= ROOT_SCAN_MS;
        nq_u32 pending = 0;
        int scan_hubs = first || !g_boot_done || (now - last_hub) >= HUB_POLL_MS;

        for (nq_u32 i = 0; i < controllers; i++) {
            xhci64_t* hc = xhci64_get(i);

            xhci64_poll(hc);

            if (xhci64_take_port_event(hc)) {
                scan_root = 1;
            }
        }

        g_attached_this_pass = 0;

        if (scan_root) {
            for (nq_u32 i = 0; i < controllers; i++) {
                pending += root_scan(i);
            }

            last_root = ms_now();
        }

        for (nq_u32 i = 0; i < controllers; i++) {
            xhci64_recover(xhci64_get(i));
        }

        if (scan_hubs) {
            for (nq_u32 i = 0; i < USB64_MAX_DEVICES; i++) {
                if (g_dev[i].used && (g_dev[i].kinds & KIND_HUB)) {
                    hub_poll((int)i);
                }
            }

            last_hub = ms_now();
        }

        /*
         * Boot is "done" after the first pass that found nothing new:
         * everything plugged in at power-on is then set up.
         */
        if (!g_boot_done && !first && g_attached_this_pass == 0 && pending == 0 &&
            (now - started) >= BOOT_SETTLE_MS) {
            g_boot_done = 1;
        }

        first = 0;

        usbhid64_tick();

        xhci64_task_sleep(g_boot_done ? 2 : 1);
    }
}

void usb64_init(void) {
    if (g_started) {
        return;
    }

    g_started = 1;

    if (process64_create_kernel_task_ex("usb", usb_task, 0, 0) <= 0) {
        usb64_log("USB: could not start the USB task\n");
        g_boot_done = 1;
    }
}

void usb64_boot_report(void) {
    nq_u64 start = ms_now();

    while (g_started && !g_boot_done && (ms_now() - start) < 4000u) {
        process64_sleep_ticks(10);
    }

    if (g_log_len) {
        k64_print(g_log);
        k64_print("\n");
    }
}

static const char* speed_name(nq_u8 speed) {
    switch (speed) {
        case XHCI64_SPEED_LOW:  return "low";
        case XHCI64_SPEED_FULL: return "full";
        case XHCI64_SPEED_HIGH: return "high";
        default:                return "super";
    }
}

void usb64_print_status(void) {
    nq_u32 count = 0;

    xhci64_print_status();

    for (nq_u32 i = 0; i < USB64_MAX_DEVICES; i++) {
        const usb64_device_t* d = &g_dev[i];

        if (!d->used) {
            continue;
        }

        count++;

        k64_print("  ");

        for (nq_u32 t = 0; t < d->tier; t++) {
            k64_print("  ");
        }

        k64_print("port ");
        k64_print_dec64(d->tier ? d->parent_port : d->root_port);
        k64_print(": ");

        if (d->kinds & KIND_HUB) {
            k64_print("hub, ");
            k64_print_dec64(d->hub_ports);
            k64_print(" ports");
        } else if (d->kinds & KIND_STORAGE) {
            k64_print("storage");
        } else if (d->kinds & KIND_BLUETOOTH) {
            k64_print("Bluetooth");
        } else if ((d->kinds & KIND_KEYBOARD) && (d->kinds & KIND_POINTER)) {
            k64_print("keyboard + mouse");
        } else if (d->kinds & KIND_KEYBOARD) {
            k64_print("keyboard");
        } else if (d->kinds & KIND_POINTER) {
            k64_print("mouse");
        } else {
            k64_print("device (no driver)");
        }

        k64_print("  [");
        k64_print(speed_name(d->speed));
        k64_print(" speed, controller ");
        k64_print_dec64(d->hc_index);
        k64_print("]\n");
    }

    if (!count && xhci64_count()) {
        k64_print("  no USB devices\n");
    }

    usbhid64_print_status();
    usbmsc64_print_status();

    if (g_log_len) {
        k64_print("\nLog:\n");
        k64_print(g_log);
    }
}
