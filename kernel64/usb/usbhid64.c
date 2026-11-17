#include "usbhid64.h"
#include "usb64.h"

#include "../k64_console.h"
#include "../input/kinput64.h"
#include "../input/mouse64.h"
#include "../arch/x86_64/timer64.h"

/*
 * USB HID keyboards and pointers. See usbhid64.h.
 *
 * Keyboards and most mice support the "boot protocol": a fixed, simple
 * report layout meant for BIOSes. We ask for it whenever it is offered,
 * which also sidesteps the per-device quirks of full HID reports:
 *
 *   keyboard: [modifiers] [reserved] [up to 6 key codes currently held]
 *   mouse:    [buttons] [dx] [dy]
 *
 * Pointers without the boot protocol (the absolute "tablet" of virtual
 * machines, some touchpads) describe their reports in a HID report
 * descriptor; parse_pointer() reads just enough of that to find the
 * buttons and the X/Y fields.
 */

#define HID_MAX_DEVICES 16u

#define REQ_GET_DESCRIPTOR 6u
#define REQ_SET_REPORT     0x09u
#define REQ_SET_IDLE       0x0Au
#define REQ_SET_PROTOCOL   0x0Bu

#define KEY_REPEAT_DELAY_MS 400u
#define KEY_REPEAT_RATE_MS  35u

#define MOD_CTRL  0x11u
#define MOD_SHIFT 0x22u
#define MOD_ALT   0x44u

#define USAGE_TAB 0x2Bu

#define USAGE_CAPS_LOCK 0x39u

typedef struct pointer_layout {
    int has_id;
    nq_u8 id;

    int has_x;
    int has_y;
    int absolute;

    nq_u32 x_off;
    nq_u32 x_bits;
    nq_s32 x_min;
    nq_s32 x_max;

    nq_u32 y_off;
    nq_u32 y_bits;
    nq_s32 y_min;
    nq_s32 y_max;

    /*
     * Bit position of buttons 1..3, or -1.
     */
    int button_off[3];

    int has_wheel;
    nq_u32 wheel_off;
    nq_u32 wheel_bits;
} pointer_layout_t;

typedef struct hid_dev {
    int used;

    xhci64_t* hc;
    nq_u8 slot;
    nq_u8 interface;

    int kind;
    int boot;

    /*
     * Keyboard: what was held in the previous report.
     */
    nq_u8 keys[6];
    nq_u8 mods;

    pointer_layout_t layout;
    nq_u8 buttons;

    nq_u64 reports;
} hid_dev_t;

static hid_dev_t g_hid[HID_MAX_DEVICES];

static nq_u8 g_report_desc[1024];

static int g_caps_lock = 0;
static int g_leds_dirty = 0;

/*
 * Software key repeat: USB keyboards only say which keys are down.
 */
static hid_dev_t* g_repeat_dev = 0;
static nq_u8 g_repeat_usage = 0;
static nq_u64 g_repeat_at = 0;

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

static nq_u64 ms_now(void) {
    nq_u64 hz = timer64_get_frequency();

    if (!hz) {
        hz = 1000u;
    }

    return (timer64_get_ticks() * 1000u) / hz;
}

/*
 * ------------------------------------------------------------
 * Keyboard
 * ------------------------------------------------------------
 */

/*
 * HID keyboard usage -> character, for usages 0x04..0x38.
 */
static const char g_plain[] =
    "abcdefghijklmnopqrstuvwxyz"
    "1234567890"
    "\n\x1B\b\t "
    "-=[]\\\\;'`,./";

static const char g_shifted[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!@#$%^&*()"
    "\n\x1B\b\t "
    "_+{}||:\"~<>?";

static char key_to_char(nq_u8 usage, nq_u8 mods) {
    int shift = (mods & MOD_SHIFT) != 0;
    int ctrl = (mods & MOD_CTRL) != 0;

    if (usage == USAGE_TAB && (mods & MOD_ALT)) {
        return (char)KINPUT64_KEY_ALT_TAB;
    }

    if (usage >= 0x04u && usage <= 0x38u) {
        int letter = usage <= 0x1Du;
        int upper = letter ? (shift != (g_caps_lock != 0)) : shift;
        char c = upper ? g_shifted[usage - 0x04u] : g_plain[usage - 0x04u];

        /*
         * Ctrl+letter is the classic control code (Ctrl+A = 1 ...), the
         * same as the PS/2 driver produces.
         */
        if (ctrl && letter) {
            c = (char)(usage - 0x04u + 1u);
        }

        return c;
    }

    switch (usage) {
        case 0x4A: return (char)KINPUT64_KEY_HOME;
        case 0x4B: return (char)KINPUT64_KEY_PAGE_UP;
        case 0x4C: return (char)KINPUT64_KEY_DELETE;
        case 0x4D: return (char)KINPUT64_KEY_END;
        case 0x4E: return (char)KINPUT64_KEY_PAGE_DOWN;
        case 0x4F: return (char)KINPUT64_KEY_RIGHT;
        case 0x50: return (char)KINPUT64_KEY_LEFT;
        case 0x51: return (char)KINPUT64_KEY_DOWN;
        case 0x52: return (char)KINPUT64_KEY_UP;

        /*
         * Keypad, treated as if Num Lock were always on.
         */
        case 0x54: return '/';
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x58: return '\n';
        case 0x62: return '0';
        case 0x63: return '.';

        /*
         * The extra key next to left Shift on ISO keyboards.
         */
        case 0x64: return shift ? '|' : '\\';

        default: break;
    }

    if (usage >= 0x59u && usage <= 0x61u) {
        return (char)('1' + (usage - 0x59u));
    }

    return 0;
}

static void push_key(char c) {
    if (!c) {
        return;
    }

    nq_u64 flags = irq_save_disable();

    kinput64_push_char(c);

    irq_restore(flags);
}

static void key_pressed(hid_dev_t* h, nq_u8 usage) {
    if (usage == USAGE_CAPS_LOCK) {
        g_caps_lock = !g_caps_lock;
        g_leds_dirty = 1;
        return;
    }

    char c = key_to_char(usage, h->mods);

    if (!c) {
        return;
    }

    push_key(c);

    g_repeat_dev = h;
    g_repeat_usage = usage;
    g_repeat_at = ms_now() + KEY_REPEAT_DELAY_MS;
}

static void keyboard_report(hid_dev_t* h, const nq_u8* data, nq_u32 length) {
    nq_u8 now[6];

    if (length < 3u) {
        return;
    }

    for (nq_u32 i = 0; i < 6u; i++) {
        now[i] = (i + 2u < length) ? data[i + 2u] : 0;
    }

    /*
     * Usages 1..3 are error codes ("too many keys held"), not keys.
     */
    if (now[0] >= 1u && now[0] <= 3u) {
        return;
    }

    h->mods = data[0];

    for (nq_u32 i = 0; i < 6u; i++) {
        nq_u8 usage = now[i];
        int held = 0;

        if (usage < 4u) {
            continue;
        }

        for (nq_u32 j = 0; j < 6u; j++) {
            if (h->keys[j] == usage) {
                held = 1;
                break;
            }
        }

        if (!held) {
            key_pressed(h, usage);
        }
    }

    if (g_repeat_dev == h && g_repeat_usage) {
        int still = 0;

        for (nq_u32 i = 0; i < 6u; i++) {
            if (now[i] == g_repeat_usage) {
                still = 1;
                break;
            }
        }

        if (!still) {
            g_repeat_usage = 0;
            g_repeat_dev = 0;
        }
    }

    for (nq_u32 i = 0; i < 6u; i++) {
        h->keys[i] = now[i];
    }
}

/*
 * ------------------------------------------------------------
 * Pointer
 * ------------------------------------------------------------
 */

static void push_pointer(
    hid_dev_t* h,
    int dx,
    int dy,
    nq_u8 buttons,
    int absolute,
    nq_u32 ax,
    nq_u32 ay,
    int wheel
) {
    mouse64_event_t ev;

    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    ev.absolute = absolute;
    ev.abs_x = ax;
    ev.abs_y = ay;
    ev.wheel = wheel;

    h->buttons = buttons;

    mouse64_push_event(&ev);
}

static void boot_mouse_report(hid_dev_t* h, const nq_u8* data, nq_u32 length) {
    if (length < 3u) {
        return;
    }

    nq_u8 buttons = data[0] & 0x7u;
    int dx = (nq_s8)data[1];
    int dy = (nq_s8)data[2];

    /*
     * A fourth byte, when present, is the wheel. HID counts rolling away
     * from the user as positive; we count scrolling down as positive.
     */
    int wheel = (length >= 4u) ? -(int)(nq_s8)data[3] : 0;

    if (dx || dy || wheel || buttons != h->buttons) {
        push_pointer(h, dx, dy, buttons, 0, 0, 0, wheel);
    }
}

/*
 * Reads a little-endian bit field out of a report.
 */
static nq_u32 get_bits(const nq_u8* data, nq_u32 length, nq_u32 offset, nq_u32 bits) {
    nq_u32 value = 0;

    for (nq_u32 i = 0; i < bits && i < 32u; i++) {
        nq_u32 bit = offset + i;

        if ((bit >> 3) >= length) {
            break;
        }

        if ((data[bit >> 3] >> (bit & 7u)) & 1u) {
            value |= 1u << i;
        }
    }

    return value;
}

static nq_s32 get_field(const nq_u8* data, nq_u32 length, nq_u32 offset, nq_u32 bits, int is_signed) {
    nq_u32 value = get_bits(data, length, offset, bits);

    if (is_signed && bits > 0 && bits < 32u && (value & (1u << (bits - 1u)))) {
        value |= ~0u << bits;
    }

    return (nq_s32)value;
}

static nq_u32 scale_absolute(nq_s32 value, nq_s32 min, nq_s32 max) {
    if (max <= min) {
        return 0;
    }

    if (value < min) {
        value = min;
    }

    if (value > max) {
        value = max;
    }

    return (nq_u32)((((nq_u64)(nq_u32)(value - min)) * 65535u) / (nq_u32)(max - min));
}

static void pointer_report(hid_dev_t* h, const nq_u8* data, nq_u32 length) {
    const pointer_layout_t* l = &h->layout;

    if (l->has_id) {
        if (length < 1u || data[0] != l->id) {
            return;
        }

        data++;
        length--;
    }

    nq_u8 buttons = 0;

    for (nq_u32 i = 0; i < 3u; i++) {
        if (l->button_off[i] >= 0 && get_bits(data, length, (nq_u32)l->button_off[i], 1)) {
            buttons |= (nq_u8)(1u << i);
        }
    }

    nq_s32 x = get_field(data, length, l->x_off, l->x_bits, l->x_min < 0);
    nq_s32 y = get_field(data, length, l->y_off, l->y_bits, l->y_min < 0);
    int wheel = l->has_wheel
        ? -(int)get_field(data, length, l->wheel_off, l->wheel_bits, 1)
        : 0;

    if (l->absolute) {
        push_pointer(
            h, 0, 0, buttons, 1,
            scale_absolute(x, l->x_min, l->x_max),
            scale_absolute(y, l->y_min, l->y_max),
            wheel
        );
    } else if (x || y || wheel || buttons != h->buttons) {
        push_pointer(h, x, y, buttons, 0, 0, 0, wheel);
    }
}

static void layout_reset(pointer_layout_t* l) {
    nq_u8* p = (nq_u8*)l;

    for (nq_u32 i = 0; i < sizeof(*l); i++) {
        p[i] = 0;
    }

    l->button_off[0] = -1;
    l->button_off[1] = -1;
    l->button_off[2] = -1;
}

static void layout_copy(pointer_layout_t* dst, const pointer_layout_t* src) {
    nq_u8* d = (nq_u8*)dst;
    const nq_u8* s = (const nq_u8*)src;

    for (nq_u32 i = 0; i < sizeof(*dst); i++) {
        d[i] = s[i];
    }
}

/*
 * Finds the buttons and X/Y fields in a HID report descriptor.
 *
 * The descriptor is a stream of small items. "Global" items (usage page,
 * field size, field count, value range, report ID) stay in force until
 * changed; "local" items (usages) apply to the next "main" item; and each
 * Input main item appends fields to the report. We walk the stream
 * keeping count of the bit position, and note where the fields we want
 * land. Only fields inside a Mouse or Pointer application collection
 * count, so a gamepad's sticks are not mistaken for a mouse.
 *
 * Returns 1 if a report with both X and Y was found.
 */
static int parse_pointer(const nq_u8* d, nq_u32 length, pointer_layout_t* out) {
    pointer_layout_t cur;
    int found = 0;

    nq_u32 usage_page = 0;
    nq_s32 logical_min = 0;
    nq_s32 logical_max = 0;
    nq_u32 report_size = 0;
    nq_u32 report_count = 0;

    nq_u32 usages[16];
    nq_u32 usage_count = 0;
    nq_u32 usage_min = 0;
    nq_u32 usage_max = 0;
    int have_range = 0;

    nq_u32 bit = 0;
    nq_u32 depth = 0;
    int in_pointer = 0;

    nq_u32 i = 0;

    layout_reset(&cur);

    while (i < length) {
        nq_u8 prefix = d[i];

        if (prefix == 0xFEu) {
            /*
             * Long item: skip.
             */
            if (i + 2u >= length) {
                break;
            }

            i += 3u + d[i + 1u];
            continue;
        }

        nq_u32 size = prefix & 0x3u;
        nq_u32 type = (prefix >> 2) & 0x3u;
        nq_u32 tag = prefix >> 4;

        if (size == 3u) {
            size = 4u;
        }

        if (i + 1u + size > length) {
            break;
        }

        nq_u32 raw = 0;

        for (nq_u32 n = 0; n < size; n++) {
            raw |= (nq_u32)d[i + 1u + n] << (8u * n);
        }

        nq_s32 value = (nq_s32)raw;

        if (size == 1u) {
            value = (nq_s8)raw;
        } else if (size == 2u) {
            value = (nq_s16)raw;
        }

        i += 1u + size;

        if (type == 1u) {
            switch (tag) {
                case 0x0: usage_page = raw; break;
                case 0x1: logical_min = value; break;
                case 0x2: logical_max = value; break;
                case 0x7: report_size = raw; break;
                case 0x9: report_count = raw; break;

                case 0x8:
                    /*
                     * A new report starts: its fields are counted from
                     * bit 0 again (after the ID byte).
                     */
                    if (!found && cur.has_x && cur.has_y) {
                        layout_copy(out, &cur);
                        found = 1;
                    }

                    layout_reset(&cur);
                    cur.has_id = 1;
                    cur.id = (nq_u8)raw;
                    bit = 0;
                    break;

                default: break;
            }
        } else if (type == 2u) {
            nq_u32 usage = (size == 4u) ? raw : (raw & 0xFFFFu);

            if (tag == 0x0) {
                if (usage_count < 16u) {
                    usages[usage_count++] = usage;
                }
            } else if (tag == 0x1) {
                usage_min = usage;
                have_range = 1;
            } else if (tag == 0x2) {
                usage_max = usage;
                have_range = 1;
            }
        } else if (type == 0u) {
            if (tag == 0xA) {
                /*
                 * Collection. An application collection at the top level
                 * says what kind of device this part describes.
                 */
                if (depth == 0 && raw == 1u) {
                    nq_u32 usage = usage_count ? usages[0] : 0;

                    if (!(usage >> 16)) {
                        usage |= usage_page << 16;
                    }

                    in_pointer = (usage == 0x00010001u || usage == 0x00010002u);
                }

                depth++;
            } else if (tag == 0xC) {
                if (depth) {
                    depth--;
                }

                if (depth == 0) {
                    in_pointer = 0;
                }
            } else if (tag == 0x8) {
                /*
                 * Input: report_count fields of report_size bits each.
                 */
                for (nq_u32 n = 0; n < report_count; n++) {
                    nq_u32 usage = 0;

                    if (usage_count) {
                        usage = usages[n < usage_count ? n : usage_count - 1u];
                    } else if (have_range) {
                        usage = usage_min + n;

                        if (usage > usage_max) {
                            usage = usage_max;
                        }
                    }

                    if (!(usage >> 16)) {
                        usage |= usage_page << 16;
                    }

                    nq_u32 page = usage >> 16;
                    nq_u32 id = usage & 0xFFFFu;

                    /*
                     * Bit 0 of the item marks padding ("constant").
                     */
                    if (!(raw & 0x1u) && in_pointer) {
                        if (page == 9u && id >= 1u && id <= 3u) {
                            cur.button_off[id - 1u] = (int)bit;
                        } else if (page == 1u && id == 0x30u && !cur.has_x) {
                            cur.has_x = 1;
                            cur.x_off = bit;
                            cur.x_bits = report_size;
                            cur.x_min = logical_min;
                            cur.x_max = logical_max;
                            cur.absolute = !(raw & 0x4u);
                        } else if (page == 1u && id == 0x31u && !cur.has_y) {
                            cur.has_y = 1;
                            cur.y_off = bit;
                            cur.y_bits = report_size;
                            cur.y_min = logical_min;
                            cur.y_max = logical_max;
                        } else if (page == 1u && id == 0x38u && !cur.has_wheel) {
                            cur.has_wheel = 1;
                            cur.wheel_off = bit;
                            cur.wheel_bits = report_size;
                        }
                    }

                    bit += report_size;
                }
            }

            /*
             * Local items end with every main item.
             */
            usage_count = 0;
            have_range = 0;
        }
    }

    if (!found && cur.has_x && cur.has_y) {
        layout_copy(out, &cur);
        found = 1;
    }

    return found;
}

/*
 * ------------------------------------------------------------
 * Attach / detach
 * ------------------------------------------------------------
 */

static void on_report(void* ctx, const nq_u8* data, nq_u32 length) {
    hid_dev_t* h = (hid_dev_t*)ctx;

    if (!h || !h->used) {
        return;
    }

    h->reports++;

    if (h->kind == USBHID64_KEYBOARD) {
        keyboard_report(h, data, length);
    } else if (h->boot) {
        boot_mouse_report(h, data, length);
    } else {
        pointer_report(h, data, length);
    }
}

static int class_request(hid_dev_t* h, nq_u8 request, nq_u16 value) {
    return xhci64_control(h->hc, h->slot, 0x21u, request, value, h->interface, 0, 0) >= 0;
}

static int load_pointer_layout(hid_dev_t* h, nq_u16 report_length) {
    nq_u32 length = report_length;

    if (length == 0) {
        return 0;
    }

    if (length > sizeof(g_report_desc)) {
        length = sizeof(g_report_desc);
    }

    int got = xhci64_control(
        h->hc,
        h->slot,
        0x81u,
        REQ_GET_DESCRIPTOR,
        0x2200u,
        h->interface,
        g_report_desc,
        (nq_u16)length
    );

    if (got <= 0) {
        return 0;
    }

    return parse_pointer(g_report_desc, (nq_u32)got, &h->layout);
}

int usbhid64_attach(xhci64_t* hc, nq_u8 slot, const usbhid64_interface_t* intf) {
    hid_dev_t* h = 0;

    if (!hc || !intf || !intf->ep_address) {
        return USBHID64_NONE;
    }

    for (nq_u32 i = 0; i < HID_MAX_DEVICES; i++) {
        if (!g_hid[i].used) {
            h = &g_hid[i];
            break;
        }
    }

    if (!h) {
        return USBHID64_NONE;
    }

    nq_u8* p = (nq_u8*)h;

    for (nq_u32 i = 0; i < sizeof(*h); i++) {
        p[i] = 0;
    }

    h->hc = hc;
    h->slot = slot;
    h->interface = intf->number;

    /*
     * SET_IDLE 0: report only when something changes. Optional for the
     * device, so a refusal is fine.
     */
    class_request(h, REQ_SET_IDLE, 0);

    if (intf->subclass == 1u && intf->protocol == 1u) {
        h->kind = USBHID64_KEYBOARD;
        h->boot = 1;

        class_request(h, REQ_SET_PROTOCOL, 0);
    } else if (intf->subclass == 1u && intf->protocol == 2u) {
        h->kind = USBHID64_POINTER;

        if (class_request(h, REQ_SET_PROTOCOL, 0)) {
            h->boot = 1;
        } else if (!load_pointer_layout(h, intf->report_length)) {
            /*
             * Neither worked; boot-style reports are the best guess.
             */
            h->boot = 1;
        }
    } else if (load_pointer_layout(h, intf->report_length)) {
        h->kind = USBHID64_POINTER;
    } else {
        return USBHID64_NONE;
    }

    h->used = 1;

    if (!xhci64_interrupt_in_start(hc, slot, intf->ep_address, on_report, h)) {
        h->used = 0;
        return USBHID64_NONE;
    }

    if (h->kind == USBHID64_KEYBOARD) {
        g_leds_dirty = 1;
    }

    return h->kind;
}

void usbhid64_detach(xhci64_t* hc, nq_u8 slot) {
    for (nq_u32 i = 0; i < HID_MAX_DEVICES; i++) {
        hid_dev_t* h = &g_hid[i];

        if (!h->used || h->hc != hc || h->slot != slot) {
            continue;
        }

        if (g_repeat_dev == h) {
            g_repeat_dev = 0;
            g_repeat_usage = 0;
        }

        /*
         * Do not leave a button held down by a mouse that is gone.
         */
        if (h->kind == USBHID64_POINTER && h->buttons && !h->layout.absolute) {
            push_pointer(h, 0, 0, 0, 0, 0, 0, 0);
        }

        h->used = 0;
    }
}

void usbhid64_tick(void) {
    if (g_repeat_dev && g_repeat_usage && g_repeat_dev->used) {
        nq_u64 now = ms_now();

        if (now >= g_repeat_at) {
            push_key(key_to_char(g_repeat_usage, g_repeat_dev->mods));
            g_repeat_at = now + KEY_REPEAT_RATE_MS;
        }
    }

    if (g_leds_dirty) {
        /*
         * Output report: bit 0 Num Lock, bit 1 Caps Lock.
         */
        nq_u8 leds = (nq_u8)(0x1u | (g_caps_lock ? 0x2u : 0u));

        g_leds_dirty = 0;

        for (nq_u32 i = 0; i < HID_MAX_DEVICES; i++) {
            hid_dev_t* h = &g_hid[i];

            if (h->used && h->kind == USBHID64_KEYBOARD) {
                xhci64_control(h->hc, h->slot, 0x21u, REQ_SET_REPORT, 0x0200u, h->interface, &leds, 1);
            }
        }
    }
}

void usbhid64_print_status(void) {
    for (nq_u32 i = 0; i < HID_MAX_DEVICES; i++) {
        const hid_dev_t* h = &g_hid[i];

        if (!h->used) {
            continue;
        }

        k64_print("  ");

        if (h->kind == USBHID64_KEYBOARD) {
            k64_print("keyboard");
        } else if (h->boot) {
            k64_print("mouse");
        } else if (h->layout.absolute) {
            k64_print("absolute pointer");
        } else {
            k64_print("mouse (report protocol)");
        }

        k64_print(": ");
        k64_print_dec64(h->reports);
        k64_print(" reports received\n");
    }
}
