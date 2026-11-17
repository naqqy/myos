#include "boot_diag64.h"

#include "../k64_console.h"
#include "../safety/safe_mode64.h"
#include "../fs/vfs64.h"
#include "../storage/block64.h"
#include "../proc/process64.h"

static const char* fb_format_name(nq_u32 fmt) {
    /*
     * UEFI GOP:
     * 0 = RGBx
     * 1 = BGRx
     * 2 = BitMask
     * 3 = BltOnly
     */
    switch (fmt) {
        case 0:
            return "RGBx";

        case 1:
            return "BGRx";

        case 2:
            return "BitMask";

        case 3:
            return "BltOnly";

        default:
            return "Unknown";
    }
}

void boot_diag64_print(const naq_boot_info_t* boot) {
    k64_print("\n");
    k64_print("NAQ OS kernel64\n");
    k64_print("----------------\n");

    k64_print("SAFE_HW: ");
    k64_print(safe_mode64_enabled() ? "ON" : "OFF");
    k64_print("\n");

    if (boot) {
        k64_print("Framebuffer: ");
        k64_print_dec64(boot->framebuffer_width);
        k64_print("x");
        k64_print_dec64(boot->framebuffer_height);
        k64_print(" ");
        k64_print(fb_format_name(boot->framebuffer_pixel_format));
        k64_print(" pitch=");
        k64_print_dec64(boot->framebuffer_pixels_per_scanline);
        k64_print("\n");
    }

    k64_print("Keyboard: PS/2 IRQ path enabled\n");

    k64_print("VFS root: ");
    k64_print(vfs64_backend_name(vfs64_root_backend()));
    k64_print("\n");

    k64_print("Block devices: ");
    if (safe_mode64_enabled()) {
        k64_print("not probed in SAFE_HW\n");
    } else {
        k64_print_dec64(block64_device_count());
        k64_print(" registered\n");
    }

    k64_print("Disk writes: ");
    k64_print(safe_mode64_disk_writes_allowed()
        ? "UNLOCKED"
        : "LOCKED, except the USB stick the system started from");
    k64_print("\n");

    k64_print("Scheduler: process64 ready, ");
    k64_print(process64_preemption_enabled() ? "preemption ON" : "preemption OFF");
    k64_print("\n");

    k64_print("Apps: ELF64 + int 0x80 syscalls ready\n");

    k64_print("----------------\n\n");
}