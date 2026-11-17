#include "pci64.h"
#include "../k64_console.h"

/*
 * PCI configuration space scanner.
 *
 * This is the first piece of real USB keyboard support. Modern USB keyboards
 * usually sit behind an xHCI controller:
 *
 *   class    0x0C = Serial bus controller
 *   subclass 0x03 = USB controller
 *   prog_if  0x30 = xHCI
 */

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static int g_pci64_ready = 0;
static nq_u32 g_pci64_device_count = 0;
static nq_u32 g_pci64_usb_count = 0;
static nq_u32 g_pci64_xhci_count = 0;
static pci64_device_t g_first_xhci;
static int g_have_first_xhci = 0;

static inline void outl64(nq_u16 port, nq_u32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline nq_u32 inl64(nq_u16 port) {
    nq_u32 value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

nq_u32 pci64_config_read32(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset) {
    nq_u32 address;

    address =
        0x80000000u |
        ((nq_u32)bus << 16) |
        ((nq_u32)device << 11) |
        ((nq_u32)function << 8) |
        ((nq_u32)offset & 0xFCu);

    outl64(PCI_CONFIG_ADDR, address);

    return inl64(PCI_CONFIG_DATA);
}

nq_u16 pci64_config_read16(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset) {
    nq_u32 value = pci64_config_read32(bus, device, function, offset);

    return (nq_u16)((value >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

nq_u8 pci64_config_read8(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset) {
    nq_u32 value = pci64_config_read32(bus, device, function, offset);

    return (nq_u8)((value >> ((offset & 3u) * 8u)) & 0xFFu);
}

void pci64_config_write32(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset, nq_u32 value) {
    nq_u32 address;

    address =
        0x80000000u |
        ((nq_u32)bus << 16) |
        ((nq_u32)device << 11) |
        ((nq_u32)function << 8) |
        ((nq_u32)offset & 0xFCu);

    outl64(PCI_CONFIG_ADDR, address);
    outl64(PCI_CONFIG_DATA, value);
}

void pci64_config_write16(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset, nq_u16 value) {
    nq_u32 shift = (offset & 2u) * 8u;
    nq_u32 current = pci64_config_read32(bus, device, function, offset);

    current &= ~(0xFFFFu << shift);
    current |= (nq_u32)value << shift;

    pci64_config_write32(bus, device, function, offset, current);
}

static int pci64_read_device(nq_u8 bus, nq_u8 device, nq_u8 function, pci64_device_t* out) {
    nq_u16 vendor = pci64_config_read16(bus, device, function, 0x00);

    if (vendor == 0xFFFFu) {
        return 0;
    }

    if (!out) {
        return 1;
    }

    out->bus = bus;
    out->device = device;
    out->function = function;

    out->vendor_id = vendor;
    out->device_id = pci64_config_read16(bus, device, function, 0x02);

    out->revision = pci64_config_read8(bus, device, function, 0x08);
    out->prog_if = pci64_config_read8(bus, device, function, 0x09);
    out->subclass = pci64_config_read8(bus, device, function, 0x0A);
    out->class_code = pci64_config_read8(bus, device, function, 0x0B);

    out->header_type = pci64_config_read8(bus, device, function, 0x0E);

    return 1;
}

void pci64_print_device(const pci64_device_t* dev) {
    if (!dev) {
        return;
    }

    k64_print("PCI ");
    k64_print_dec64(dev->bus);
    k64_print(":");
    k64_print_dec64(dev->device);
    k64_print(".");
    k64_print_dec64(dev->function);

    k64_print(" vendor=");
    k64_print_hex64(dev->vendor_id);
    k64_print(" device=");
    k64_print_hex64(dev->device_id);

    k64_print(" class=");
    k64_print_hex64(dev->class_code);
    k64_print(" subclass=");
    k64_print_hex64(dev->subclass);
    k64_print(" prog_if=");
    k64_print_hex64(dev->prog_if);
    k64_print("\n");
}

static void pci64_scan_function(nq_u8 bus, nq_u8 device, nq_u8 function) {
    pci64_device_t dev;

    if (!pci64_read_device(bus, device, function, &dev)) {
        return;
    }

    g_pci64_device_count++;

    if (dev.class_code == 0x0Cu && dev.subclass == 0x03u) {
        g_pci64_usb_count++;


        if (dev.prog_if == 0x30u) {
            g_pci64_xhci_count++;

            if (!g_have_first_xhci) {
                g_first_xhci = dev;
                g_have_first_xhci = 1;
            }

        }
    }
}

static void pci64_scan_device(nq_u8 bus, nq_u8 device) {
    pci64_device_t dev;

    if (!pci64_read_device(bus, device, 0, &dev)) {
        return;
    }

    pci64_scan_function(bus, device, 0);

    if (dev.header_type & 0x80u) {
        for (nq_u8 function = 1; function < 8; function++) {
            pci64_scan_function(bus, device, function);
        }
    }
}

void pci64_init(void) {
    if (g_pci64_ready) {
        return;
    }

    g_pci64_device_count = 0;
    g_pci64_usb_count = 0;
    g_pci64_xhci_count = 0;
    g_have_first_xhci = 0;

    /*
     * Brute-force scan all buses. It is simple and good enough for early
     * hardware bring-up.
     */
    for (nq_u32 bus = 0; bus < 256; bus++) {
        for (nq_u8 device = 0; device < 32; device++) {
            pci64_scan_device((nq_u8)bus, device);
        }
    }

    g_pci64_ready = 1;
}

void pci64_print_summary(void) {
    k64_print("PCI64: devices=");
    k64_print_dec64(g_pci64_device_count);
    k64_print(" usb=");
    k64_print_dec64(g_pci64_usb_count);
    k64_print(" xhci=");
    k64_print_dec64(g_pci64_xhci_count);
    k64_print("\n");
}

int pci64_find_class(
    nq_u8 class_code,
    nq_u8 subclass,
    nq_u8 prog_if,
    pci64_device_t* out
) {
    return pci64_find_class_nth(class_code, subclass, prog_if, 0, out);
}

int pci64_find_class_nth(
    nq_u8 class_code,
    nq_u8 subclass,
    nq_u8 prog_if,
    nq_u32 index,
    pci64_device_t* out
) {
    pci64_device_t dev;

    if (!g_pci64_ready) {
        pci64_init();
    }

    for (nq_u32 bus = 0; bus < 256; bus++) {
        for (nq_u8 device = 0; device < 32; device++) {
            if (!pci64_read_device((nq_u8)bus, device, 0, &dev)) {
                continue;
            }

            nq_u8 max_function = (dev.header_type & 0x80u) ? 8u : 1u;

            for (nq_u8 function = 0; function < max_function; function++) {
                if (!pci64_read_device((nq_u8)bus, device, function, &dev)) {
                    continue;
                }

                if (dev.class_code == class_code &&
                    dev.subclass == subclass &&
                    dev.prog_if == prog_if) {
                    if (index) {
                        index--;
                        continue;
                    }

                    if (out) {
                        *out = dev;
                    }

                    return 1;
                }
            }
        }
    }

    return 0;
}

int pci64_find_xhci(pci64_device_t* out) {
    if (!g_pci64_ready) {
        pci64_init();
    }

    if (g_have_first_xhci) {
        if (out) {
            *out = g_first_xhci;
        }

        return 1;
    }

    return pci64_find_class(0x0Cu, 0x03u, 0x30u, out);
}

nq_u64 pci64_read_bar(pci64_device_t dev, nq_u8 bar_index, int* out_is_mmio, int* out_is_64) {
    if (bar_index >= 6u) {
        return 0;
    }

    nq_u8 offset = (nq_u8)(0x10u + (bar_index * 4u));
    nq_u32 bar0 = pci64_config_read32(dev.bus, dev.device, dev.function, offset);

    int is_io = (bar0 & 0x1u) != 0;
    int is_64 = 0;
    nq_u64 address = 0;

    if (is_io) {
        address = (nq_u64)(bar0 & 0xFFFFFFFCu);
    } else {
        nq_u32 type = (bar0 >> 1) & 0x3u;

        if (type == 0x2u && bar_index + 1u < 6u) {
            nq_u32 bar1 = pci64_config_read32(
                dev.bus,
                dev.device,
                dev.function,
                (nq_u8)(offset + 4u)
            );

            address = ((nq_u64)bar1 << 32) | (nq_u64)(bar0 & 0xFFFFFFF0u);
            is_64 = 1;
        } else {
            address = (nq_u64)(bar0 & 0xFFFFFFF0u);
        }
    }

    if (out_is_mmio) {
        *out_is_mmio = !is_io;
    }

    if (out_is_64) {
        *out_is_64 = is_64;
    }

    return address;
}
