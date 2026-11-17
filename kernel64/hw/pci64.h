#ifndef PCI64_H
#define PCI64_H

#include "naq_boot_info.h"

#define PCI64_INVALID_U32 0xFFFFFFFFu

typedef struct pci64_device {
    nq_u8 bus;
    nq_u8 device;
    nq_u8 function;

    nq_u16 vendor_id;
    nq_u16 device_id;

    nq_u8 class_code;
    nq_u8 subclass;
    nq_u8 prog_if;
    nq_u8 revision;

    nq_u8 header_type;
} pci64_device_t;

void pci64_init(void);

nq_u32 pci64_config_read32(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset);
nq_u16 pci64_config_read16(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset);
nq_u8  pci64_config_read8(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset);

void pci64_config_write32(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset, nq_u32 value);
void pci64_config_write16(nq_u8 bus, nq_u8 device, nq_u8 function, nq_u8 offset, nq_u16 value);

int pci64_find_class(
    nq_u8 class_code,
    nq_u8 subclass,
    nq_u8 prog_if,
    pci64_device_t* out
);

/*
 * Like pci64_find_class(), but returns the index-th match (0 = first), for
 * machines with several controllers of the same kind.
 */
int pci64_find_class_nth(
    nq_u8 class_code,
    nq_u8 subclass,
    nq_u8 prog_if,
    nq_u32 index,
    pci64_device_t* out
);

int pci64_find_xhci(pci64_device_t* out);
nq_u64 pci64_read_bar(pci64_device_t dev, nq_u8 bar_index, int* out_is_mmio, int* out_is_64);

void pci64_print_device(const pci64_device_t* dev);
void pci64_print_summary(void);

#endif
