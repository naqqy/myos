#include "xhci64.h"
#include "../k64_console.h"
#include "../mm/paging64.h"
#include "../mm/vmem64.h"

static xhci64_controller_t g_xhci64;

static nq_u8 mmio_read8(nq_u64 base, nq_u32 offset) {
    volatile nq_u8* p = (volatile nq_u8*)(base + offset);

    return *p;
}

static nq_u16 mmio_read16(nq_u64 base, nq_u32 offset) {
    volatile nq_u16* p = (volatile nq_u16*)(base + offset);

    return *p;
}

static nq_u32 mmio_read32(nq_u64 base, nq_u32 offset) {
    volatile nq_u32* p = (volatile nq_u32*)(base + offset);

    return *p;
}

const xhci64_controller_t* xhci64_controller(void) {
    return &g_xhci64;
}

int xhci64_available(void) {
    return g_xhci64.present;
}

int xhci64_init(void) {
    pci64_device_t dev;
    int is_mmio = 0;
    int is_64 = 0;
    nq_u64 bar = 0;

    if (g_xhci64.present) {
        return 1;
    }

    if (!pci64_find_xhci(&dev)) {
        k64_print("XHCI64: no xHCI controller found.\n");
        return 0;
    }

    bar = pci64_read_bar(dev, 0, &is_mmio, &is_64);

    if (!bar || !is_mmio) {
        k64_print("XHCI64: invalid BAR0 for xHCI controller.\n");
        return 0;
    }

    /*
     * Map the first page of the controller MMIO registers so we can read
     * capability registers. Full controller init/rings come in the next pass.
     */
    if (!paging64_map_direct_range(bar & ~0xFFFull, 0x1000ull)) {
        k64_print("XHCI64: failed to map MMIO BAR0 page.\n");
        return 0;
    }

    g_xhci64.present = 1;
    g_xhci64.pci = dev;
    g_xhci64.mmio_phys = bar;
    g_xhci64.mmio_virt = vmem64_phys_to_direct(bar);

    g_xhci64.cap_length = mmio_read8(g_xhci64.mmio_virt, 0x00);
    g_xhci64.hci_version = mmio_read16(g_xhci64.mmio_virt, 0x02);
    g_xhci64.hcs_params1 = mmio_read32(g_xhci64.mmio_virt, 0x04);
    g_xhci64.hcc_params1 = mmio_read32(g_xhci64.mmio_virt, 0x10);

    k64_print("XHCI64: controller found.\n");
    k64_print("  BAR0 phys=");
    k64_print_hex64(g_xhci64.mmio_phys);
    k64_print(" virt=");
    k64_print_hex64(g_xhci64.mmio_virt);
    k64_print(is_64 ? " 64-bit\n" : " 32-bit\n");

    xhci64_print_summary();

    k64_print("XHCI64: capability probe only; controller start/enumeration is next.\n");

    return 1;
}

void xhci64_print_summary(void) {
    if (!g_xhci64.present) {
        k64_print("XHCI64: unavailable.\n");
        return;
    }

    k64_print("XHCI64: cap_length=");
    k64_print_dec64(g_xhci64.cap_length);
    k64_print(" version=");
    k64_print_hex64(g_xhci64.hci_version);
    k64_print(" hcs1=");
    k64_print_hex64(g_xhci64.hcs_params1);
    k64_print(" hcc1=");
    k64_print_hex64(g_xhci64.hcc_params1);
    k64_print("\n");

    k64_print("XHCI64: max slots=");
    k64_print_dec64(g_xhci64.hcs_params1 & 0xFFu);
    k64_print(" max interrupters=");
    k64_print_dec64((g_xhci64.hcs_params1 >> 8) & 0x7FFu);
    k64_print(" max ports=");
    k64_print_dec64((g_xhci64.hcs_params1 >> 24) & 0xFFu);
    k64_print("\n");
}

void xhci64_poll(void) {
    /*
     * Placeholder for 73C/73D.
     *
     * Real USB keyboard support needs command/event rings, device slots,
     * endpoint contexts, port reset, device address/configuration, HID boot
     * protocol, and interrupt endpoint polling.
     */
}
