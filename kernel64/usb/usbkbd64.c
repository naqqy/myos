#include "usbkbd64.h"
#include "xhci64.h"
#include "../k64_console.h"
#include "../input/kinput64.h"

static int g_usbkbd64_ready = 0;

void usbkbd64_init(void) {
    /*
     * 73A/73B foundation:
     * We can now discover xHCI and read its capability registers.
     *
     * Actual HID boot keyboard enumeration is intentionally not faked here.
     * The next pass will initialize xHCI rings and poll keyboard reports,
     * then call kinput64_push_char().
     */
    if (xhci64_available()) {
        g_usbkbd64_ready = 1;
        k64_print("USBKBD64: xHCI present; HID keyboard enumeration pending.\n");
    } else {
        g_usbkbd64_ready = 0;
        k64_print("USBKBD64: no xHCI backend available.\n");
    }
}

int usbkbd64_available(void) {
    return g_usbkbd64_ready;
}

void usbkbd64_poll(void) {
    if (!g_usbkbd64_ready) {
        return;
    }

    /*
     * Placeholder for 73D. This function will eventually decode USB HID boot
     * reports and inject characters with kinput64_push_char(c).
     */
    xhci64_poll();
}
