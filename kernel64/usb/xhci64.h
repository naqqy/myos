#ifndef XHCI64_H
#define XHCI64_H

#include "naq_boot_info.h"
#include "../hw/pci64.h"

typedef struct xhci64_controller {
    int present;
    pci64_device_t pci;

    nq_u64 mmio_phys;
    nq_u64 mmio_virt;

    nq_u8 cap_length;
    nq_u16 hci_version;
    nq_u32 hcs_params1;
    nq_u32 hcc_params1;
} xhci64_controller_t;

int xhci64_init(void);
int xhci64_available(void);
const xhci64_controller_t* xhci64_controller(void);

void xhci64_print_summary(void);

/*
 * Placeholder for the next pass.
 */
void xhci64_poll(void);

#endif
