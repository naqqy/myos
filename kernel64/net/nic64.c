#include "nic64.h"

#include "../hw/pci64.h"
#include "../mm/paging64.h"
#include "../mm/pmm64.h"
#include "../mm/vmem64.h"
#include "../arch/x86_64/timer64.h"
#include "../proc/process64.h"

/*
 * Intel igb/igc Ethernet driver. See nic64.h.
 *
 * The card and the driver share two rings of 16-byte "descriptors" in
 * ordinary memory:
 *
 *   receive ring   each descriptor points at an empty 2 KiB buffer. The
 *                  card fills buffers with incoming frames and marks the
 *                  descriptor done; the driver takes the frame and hands
 *                  the buffer back.
 *   transmit ring  the driver points a descriptor at a frame and moves
 *                  the "tail" register past it; the card sends it.
 *
 * All memory handed to the card comes from the physical frame allocator,
 * which is identity mapped and below 4 GiB, so a pointer is also the
 * physical address the card needs.
 */

#define RING_SIZE    64u
#define BUFFER_SIZE  2048u

#define MMIO_MAP_SIZE 0x100000ull

/*
 * Registers.
 */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EECD    0x0010
#define REG_MDIC    0x0020
#define REG_ICR     0x00C0
#define REG_IMC     0x00D8
#define REG_EIMC    0x1528
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define REG_RDBAL   0xC000
#define REG_RDBAH   0xC004
#define REG_RDLEN   0xC008
#define REG_SRRCTL  0xC00C
#define REG_RDH     0xC010
#define REG_RDT     0xC018
#define REG_RXDCTL  0xC028

#define REG_TDBAL   0xE000
#define REG_TDBAH   0xE004
#define REG_TDLEN   0xE008
#define REG_TDH     0xE010
#define REG_TDT     0xE018
#define REG_TXDCTL  0xE028

#define CTRL_MASTER_DISABLE 0x00000004u
#define CTRL_SLU            0x00000040u   /* set link up */
#define CTRL_FRCSPD         0x00000800u
#define CTRL_FRCDPX         0x00001000u
#define CTRL_RST            0x04000000u
#define CTRL_PHY_RST        0x80000000u

#define STATUS_LINK_UP      0x00000002u
#define STATUS_MASTER_ON    0x00080000u
#define STATUS_SPEED_2500   0x00400000u

#define EECD_AUTO_READ_DONE 0x00000200u

#define RCTL_ENABLE         0x00000002u
#define RCTL_BROADCAST      0x00008000u
#define RCTL_STRIP_CRC      0x04000000u

#define TCTL_ENABLE         0x00000002u
#define TCTL_PAD_SHORT      0x00000008u

#define QUEUE_ENABLE        0x02000000u

/*
 * Receive buffers are 2 KiB, "advanced" one-buffer descriptors.
 */
#define SRRCTL_VALUE        0x02000002u

#define RX_STATUS_DONE      0x00000001u
#define RX_STATUS_END       0x00000002u

#define TX_CMD_END_OF_PACKET 0x01000000u
#define TX_CMD_INSERT_CRC    0x02000000u
#define TX_CMD_REPORT_STATUS 0x08000000u
#define TX_CMD_ADVANCED      0x20000000u
#define TX_TYPE_DATA         0x00300000u
#define TX_STATUS_DONE       0x00000001u

#define MDIC_READY          0x10000000u
#define MDIC_ERROR          0x40000000u

/*
 * A descriptor as two 64-bit words. For receive the card overwrites both
 * with status when it has filled the buffer.
 */
typedef struct nic_desc {
    nq_u64 a;
    nq_u64 b;
} nic_desc_t;

typedef struct nic_model {
    nq_u16 device_id;
    const char* name;
    nq_u8 phy_address;
} nic_model_t;

static const nic_model_t g_models[] = {
    { 0x125C, "Intel I226-V",  0 },
    { 0x125B, "Intel I226-LM", 0 },
    { 0x125D, "Intel I226-IT", 0 },
    { 0x3102, "Intel I226-K",  0 },
    { 0x15F3, "Intel I225-V",  0 },
    { 0x15F2, "Intel I225-LM", 0 },
    { 0x0D9F, "Intel I225-IT", 0 },
    { 0x3100, "Intel I225-K",  0 },
    { 0x1533, "Intel I210",    0 },
    { 0x1536, "Intel I210",    0 },
    { 0x1537, "Intel I210",    0 },
    { 0x1538, "Intel I210",    0 },
    { 0x1539, "Intel I211",    0 },
    { 0x157B, "Intel I210",    0 },
    { 0x1521, "Intel I350",    1 },
    { 0x10C9, "Intel 82576",   1 }
};

static int g_present = 0;
static const nic_model_t* g_model = 0;
static nq_u64 g_mmio = 0;
static nq_u8 g_mac[6];

static nic_desc_t* g_rx_ring = 0;
static nic_desc_t* g_tx_ring = 0;
static nq_u8* g_rx_buffer[RING_SIZE];
static nq_u8* g_tx_buffer[RING_SIZE];
static nq_u8 g_tx_used[RING_SIZE];

static nq_u32 g_rx_next = 0;
static nq_u32 g_tx_next = 0;

static nq_u64 g_rx_count = 0;
static nq_u64 g_tx_count = 0;

static nq_u32 r32(nq_u32 reg) {
    return *(volatile nq_u32*)(g_mmio + reg);
}

static void w32(nq_u32 reg, nq_u32 value) {
    *(volatile nq_u32*)(g_mmio + reg) = value;
}

static void sleep_ms(nq_u32 ms) {
    nq_u64 hz = timer64_get_frequency();
    nq_u64 ticks = ((nq_u64)ms * (hz ? hz : 1000u) + 999u) / 1000u;

    process64_sleep_ticks(ticks ? ticks : 1u);
}

/*
 * Waits for (register & mask) == want.
 */
static int wait_reg(nq_u32 reg, nq_u32 mask, nq_u32 want, nq_u32 ms) {
    for (nq_u32 waited = 0; ; waited++) {
        if ((r32(reg) & mask) == want) {
            return 1;
        }

        if (waited >= ms) {
            return 0;
        }

        sleep_ms(1);
    }
}

/*
 * 2 KiB buffers, two to a page.
 */
static int alloc_buffers(nq_u8** list) {
    for (nq_u32 i = 0; i < RING_SIZE; i += 2u) {
        nq_u8* page = (nq_u8*)pmm64_alloc_frame();

        if (!page) {
            return 0;
        }

        list[i] = page;
        list[i + 1u] = page + BUFFER_SIZE;
    }

    return 1;
}

static void* alloc_zeroed_page(void) {
    nq_u8* page = (nq_u8*)pmm64_alloc_frame();

    if (page) {
        for (nq_u32 i = 0; i < 4096u; i++) {
            page[i] = 0;
        }
    }

    return page;
}

static int start_card(const pci64_device_t* pci) {
    int is_mmio = 0;
    int is_64 = 0;
    nq_u64 bar = pci64_read_bar(*pci, 0, &is_mmio, &is_64);

    if (!bar || !is_mmio) {
        return 0;
    }

    /*
     * Memory access and bus mastering on, legacy interrupt line off (we
     * poll).
     */
    nq_u16 command = pci64_config_read16(pci->bus, pci->device, pci->function, 0x04);

    command |= 0x0006u | 0x0400u;
    pci64_config_write16(pci->bus, pci->device, pci->function, 0x04, command);

    if (!paging64_map_direct_range(bar, MMIO_MAP_SIZE)) {
        return 0;
    }

    g_mmio = vmem64_phys_to_direct(bar);

    if (r32(REG_STATUS) == 0xFFFFFFFFu) {
        return 0;
    }

    /*
     * Quiet the card, stop it using memory, then reset it.
     */
    w32(REG_IMC, 0xFFFFFFFFu);
    w32(REG_EIMC, 0xFFFFFFFFu);
    w32(REG_RCTL, 0);
    w32(REG_TCTL, TCTL_PAD_SHORT);

    w32(REG_CTRL, r32(REG_CTRL) | CTRL_MASTER_DISABLE);
    wait_reg(REG_STATUS, STATUS_MASTER_ON, 0, 100);

    w32(REG_CTRL, r32(REG_CTRL) | CTRL_RST);
    sleep_ms(10);

    if (!wait_reg(REG_CTRL, CTRL_RST, 0, 200)) {
        return 0;
    }

    /*
     * The card reloads its settings (including its address) from its own
     * flash after a reset.
     */
    wait_reg(REG_EECD, EECD_AUTO_READ_DONE, EECD_AUTO_READ_DONE, 200);

    w32(REG_IMC, 0xFFFFFFFFu);
    w32(REG_EIMC, 0xFFFFFFFFu);
    (void)r32(REG_ICR);

    /*
     * Let the transceiver negotiate speed and duplex by itself.
     */
    nq_u32 ctrl = r32(REG_CTRL);

    ctrl |= CTRL_SLU;
    ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPX | CTRL_PHY_RST | CTRL_MASTER_DISABLE);
    w32(REG_CTRL, ctrl);

    nq_u32 low = r32(REG_RAL0);
    nq_u32 high = r32(REG_RAH0);

    g_mac[0] = (nq_u8)low;
    g_mac[1] = (nq_u8)(low >> 8);
    g_mac[2] = (nq_u8)(low >> 16);
    g_mac[3] = (nq_u8)(low >> 24);
    g_mac[4] = (nq_u8)high;
    g_mac[5] = (nq_u8)(high >> 8);

    nq_u32 sum = 0;
    nq_u32 all = 0xFFu;

    for (int i = 0; i < 6; i++) {
        sum |= g_mac[i];
        all &= g_mac[i];
    }

    if (sum == 0 || all == 0xFFu) {
        return 0;
    }

    /*
     * Accept frames addressed to us (bit 31 = "this entry is valid") and
     * no multicast groups.
     */
    w32(REG_RAL0, low);
    w32(REG_RAH0, (high & 0xFFFFu) | 0x80000000u);

    for (nq_u32 i = 0; i < 128u; i++) {
        w32(REG_MTA + i * 4u, 0);
    }

    g_rx_ring = (nic_desc_t*)alloc_zeroed_page();
    g_tx_ring = (nic_desc_t*)alloc_zeroed_page();

    if (!g_rx_ring || !g_tx_ring ||
        !alloc_buffers(g_rx_buffer) || !alloc_buffers(g_tx_buffer)) {
        return 0;
    }

    /*
     * Receive.
     */
    for (nq_u32 i = 0; i < RING_SIZE; i++) {
        g_rx_ring[i].a = (nq_u64)g_rx_buffer[i];
        g_rx_ring[i].b = 0;
    }

    w32(REG_RDBAL, (nq_u32)(nq_u64)g_rx_ring);
    w32(REG_RDBAH, (nq_u32)((nq_u64)g_rx_ring >> 32));
    w32(REG_RDLEN, RING_SIZE * sizeof(nic_desc_t));
    w32(REG_SRRCTL, SRRCTL_VALUE);
    w32(REG_RDH, 0);
    w32(REG_RDT, 0);

    w32(REG_RXDCTL, r32(REG_RXDCTL) | QUEUE_ENABLE);
    wait_reg(REG_RXDCTL, QUEUE_ENABLE, QUEUE_ENABLE, 50);

    w32(REG_RCTL, RCTL_ENABLE | RCTL_BROADCAST | RCTL_STRIP_CRC);

    /*
     * The tail points one before the head: every buffer but one is the
     * card's to fill.
     */
    w32(REG_RDT, RING_SIZE - 1u);

    g_rx_next = 0;

    /*
     * Transmit.
     */
    w32(REG_TDBAL, (nq_u32)(nq_u64)g_tx_ring);
    w32(REG_TDBAH, (nq_u32)((nq_u64)g_tx_ring >> 32));
    w32(REG_TDLEN, RING_SIZE * sizeof(nic_desc_t));
    w32(REG_TDH, 0);
    w32(REG_TDT, 0);

    w32(REG_TXDCTL, r32(REG_TXDCTL) | QUEUE_ENABLE);
    wait_reg(REG_TXDCTL, QUEUE_ENABLE, QUEUE_ENABLE, 50);

    /*
     * Collision threshold 15 and collision distance 63 are the standard
     * values (they only matter on half-duplex links).
     */
    w32(REG_TCTL, TCTL_ENABLE | TCTL_PAD_SHORT | (0x0Fu << 4) | (0x3Fu << 12));

    g_tx_next = 0;

    for (nq_u32 i = 0; i < RING_SIZE; i++) {
        g_tx_used[i] = 0;
    }

    return 1;
}

int nic64_init(void) {
    if (g_present) {
        return 1;
    }

    /*
     * PCI class 2, subclass 0: Ethernet controllers.
     */
    for (nq_u32 n = 0; n < 8u; n++) {
        pci64_device_t pci;

        if (!pci64_find_class_nth(0x02u, 0x00u, 0x00u, n, &pci)) {
            break;
        }

        if (pci.vendor_id != 0x8086u) {
            continue;
        }

        for (nq_u32 m = 0; m < sizeof(g_models) / sizeof(g_models[0]); m++) {
            if (g_models[m].device_id != pci.device_id) {
                continue;
            }

            if (start_card(&pci)) {
                g_model = &g_models[m];
                g_present = 1;
                return 1;
            }
        }
    }

    return 0;
}

int nic64_present(void) {
    return g_present;
}

const char* nic64_name(void) {
    return g_model ? g_model->name : "none";
}

const nq_u8* nic64_mac(void) {
    return g_mac;
}

int nic64_link(nq_u32* out_mbps) {
    if (!g_present) {
        return 0;
    }

    nq_u32 status = r32(REG_STATUS);

    if (out_mbps) {
        nq_u32 speed = (status >> 6) & 0x3u;

        if (status & STATUS_SPEED_2500) {
            *out_mbps = 2500;
        } else if (speed == 0) {
            *out_mbps = 10;
        } else if (speed == 1) {
            *out_mbps = 100;
        } else {
            *out_mbps = 1000;
        }
    }

    return (status & STATUS_LINK_UP) != 0;
}

/*
 * The transceiver (PHY) has its own small registers, reached through the
 * card's MDIC register.
 */
static int phy_access(nq_u32 reg, int write, nq_u16 value, nq_u16* out) {
    nq_u32 command =
        (nq_u32)value |
        (reg << 16) |
        ((nq_u32)g_model->phy_address << 21) |
        (write ? (1u << 26) : (2u << 26));

    w32(REG_MDIC, command);

    for (nq_u32 i = 0; i < 2000u; i++) {
        nq_u32 result = r32(REG_MDIC);

        if (result & MDIC_READY) {
            if (result & MDIC_ERROR) {
                return 0;
            }

            if (out) {
                *out = (nq_u16)result;
            }

            return 1;
        }

        for (volatile int spin = 0; spin < 200; spin++) {
        }
    }

    return 0;
}

void nic64_restart_link(void) {
    nq_u16 control = 0;

    if (!g_present || !phy_access(0, 0, 0, &control)) {
        return;
    }

    /*
     * Control register: clear "power down" (bit 11), set "auto-negotiate"
     * (bit 12) and "restart negotiation" (bit 9).
     */
    control &= (nq_u16)~0x0800u;
    control |= 0x1200u;

    phy_access(0, 1, control, 0);
}

int nic64_upgrade_link(void) {
    nq_u16 gigabit = 0;
    nq_u16 control = 0;

    if (!g_present) {
        return 0;
    }

    /*
     * Transceiver register 9 holds what is offered for 1000BASE-T:
     * bit 9 full duplex, bit 8 half duplex. All ones means the register
     * could not really be read.
     */
    if (!phy_access(9, 0, 0, &gigabit) || gigabit == 0xFFFFu) {
        return 0;
    }

    if (!phy_access(0, 0, 0, &control) || control == 0xFFFFu) {
        return 0;
    }

    if (!phy_access(9, 1, (nq_u16)(gigabit | 0x0300u), 0)) {
        return 0;
    }

    control &= (nq_u16)~0x0800u;
    control |= 0x1200u;

    return phy_access(0, 1, control, 0);
}

int nic64_send(const void* frame, nq_u32 length) {
    if (!g_present || !frame || length < 14u || length > NIC64_MAX_FRAME) {
        return 0;
    }

    nq_u32 index = g_tx_next;
    nic_desc_t* desc = &g_tx_ring[index];

    /*
     * The slot is free once the card has marked the previous frame in it
     * as sent.
     */
    if (g_tx_used[index] && !((nq_u32)(desc->b >> 32) & TX_STATUS_DONE)) {
        return 0;
    }

    const nq_u8* src = (const nq_u8*)frame;
    nq_u8* dst = g_tx_buffer[index];

    for (nq_u32 i = 0; i < length; i++) {
        dst[i] = src[i];
    }

    /*
     * Second word: command and length in the low half, total payload
     * length (shifted by 14) in the high half.
     */
    nq_u32 command =
        length |
        TX_TYPE_DATA |
        TX_CMD_ADVANCED |
        TX_CMD_INSERT_CRC |
        TX_CMD_END_OF_PACKET |
        TX_CMD_REPORT_STATUS;

    desc->a = (nq_u64)dst;
    desc->b = (nq_u64)command | ((nq_u64)(length << 14) << 32);

    g_tx_used[index] = 1;
    g_tx_next = (index + 1u) % RING_SIZE;

    w32(REG_TDT, g_tx_next);

    g_tx_count++;

    return 1;
}

int nic64_receive(void* buffer, nq_u32 capacity, nq_u32* out_length) {
    if (!g_present || !buffer || !out_length) {
        return 0;
    }

    for (;;) {
        nq_u32 index = g_rx_next;
        nic_desc_t* desc = &g_rx_ring[index];

        /*
         * After the card fills a buffer, the second word holds status in
         * its low 32 bits and the frame length in bits 32..47.
         */
        nq_u32 status = (nq_u32)desc->b;

        if (!(status & RX_STATUS_DONE)) {
            return 0;
        }

        nq_u32 length = (nq_u32)(desc->b >> 32) & 0xFFFFu;
        int usable = (status & RX_STATUS_END) && length <= capacity && length <= BUFFER_SIZE;

        if (usable) {
            const nq_u8* src = g_rx_buffer[index];
            nq_u8* dst = (nq_u8*)buffer;

            for (nq_u32 i = 0; i < length; i++) {
                dst[i] = src[i];
            }
        }

        /*
         * Hand the buffer back and let the card use this slot again.
         */
        desc->a = (nq_u64)g_rx_buffer[index];
        desc->b = 0;

        w32(REG_RDT, index);

        g_rx_next = (index + 1u) % RING_SIZE;

        if (usable) {
            *out_length = length;
            g_rx_count++;
            return 1;
        }
    }
}

nq_u64 nic64_rx_count(void) {
    return g_rx_count;
}

nq_u64 nic64_tx_count(void) {
    return g_tx_count;
}
