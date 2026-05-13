#include "block64.h"

#include "../arch/x86_64/io64.h"
#include "../k64_console.h"
#include "../safety/safe_mode64.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_FEATURES    1
#define ATA_REG_SECCOUNT0   2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_HDDEVSEL    6
#define ATA_REG_COMMAND     7
#define ATA_REG_STATUS      7

#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_READ_SECTORS   0x20
#define ATA_CMD_WRITE_SECTORS  0x30
#define ATA_CMD_CACHE_FLUSH    0xE7

#define ATA_SR_ERR   0x01
#define ATA_SR_DRQ   0x08
#define ATA_SR_DF    0x20
#define ATA_SR_DRDY  0x40
#define ATA_SR_BSY   0x80

#define ATA_POLL_TIMEOUT 1000000u

static int g_block_ready = 0;
static nq_u64 g_sector_count = 0;

static block64_device_t g_block_devices[BLOCK64_MAX_DEVICES];
static nq_u32 g_block_device_count = 0;
static block64_device_t* g_boot_device = 0;

/* ============================================================
 * Device registry
 * ============================================================ */

static void block64_clear_devices(void) {
    for (nq_u32 i = 0; i < BLOCK64_MAX_DEVICES; i++) {
        g_block_devices[i].id = i;
        g_block_devices[i].name = "none";
        g_block_devices[i].type = BLOCK64_DEVICE_NONE;
        g_block_devices[i].sector_count = 0;
        g_block_devices[i].sector_size = BLOCK64_SECTOR_SIZE;
        g_block_devices[i].present = 0;
        g_block_devices[i].read_only = 1;
        g_block_devices[i].writes_unlocked = 0;
    }

    g_block_device_count = 0;
    g_boot_device = 0;
}

static block64_device_t* block64_register_device(
    const char* name,
    block64_device_type_t type,
    nq_u64 sector_count,
    int read_only
) {
    if (g_block_device_count >= BLOCK64_MAX_DEVICES) {
        return 0;
    }

    block64_device_t* dev = &g_block_devices[g_block_device_count];

    dev->id = g_block_device_count;
    dev->name = name ? name : "blk";
    dev->type = type;
    dev->sector_count = sector_count;
    dev->sector_size = BLOCK64_SECTOR_SIZE;
    dev->present = 1;
    dev->read_only = read_only ? 1 : 0;
    dev->writes_unlocked = 0;

    g_block_device_count++;

    if (!g_boot_device) {
        g_boot_device = dev;
    }

    return dev;
}

/* ============================================================
 * ATA helpers
 * ============================================================ */

static nq_u8 ata_read_status(void) {
    return inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
}

static nq_u8 ata_read_alt_status(void) {
    return inb(ATA_PRIMARY_CTRL);
}

static void ata_400ns_delay(void) {
    ata_read_alt_status();
    ata_read_alt_status();
    ata_read_alt_status();
    ata_read_alt_status();
}

static int ata_wait_not_busy(void) {
    for (nq_u32 i = 0; i < ATA_POLL_TIMEOUT; i++) {
        nq_u8 status = ata_read_status();

        if ((status & ATA_SR_BSY) == 0) {
            return 1;
        }
    }

    return 0;
}

static int ata_wait_drq_or_error(void) {
    for (nq_u32 i = 0; i < ATA_POLL_TIMEOUT; i++) {
        nq_u8 status = ata_read_status();

        if (status & ATA_SR_BSY) {
            continue;
        }

        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return 0;
        }

        if (status & ATA_SR_DRQ) {
            return 1;
        }
    }

    return 0;
}

static int ata_status_ok(void) {
    nq_u8 status = ata_read_status();

    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        return 0;
    }

    return 1;
}

static void ata_select_primary_master_lba28(nq_u32 lba) {
    /*
     * 0xE0 = primary master, LBA mode.
     */
    outb(
        ATA_PRIMARY_IO + ATA_REG_HDDEVSEL,
        (nq_u8)(0xE0u | ((lba >> 24) & 0x0Fu))
    );

    ata_400ns_delay();
}

static int ata_identify_primary_master(void) {
    nq_u16 identify[256];

    g_block_ready = 0;
    g_sector_count = 0;

    /*
     * Select primary master.
     */
    outb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, 0xA0);
    ata_400ns_delay();

    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, 0);

    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    nq_u8 status = ata_read_status();

    /*
     * 0 or 0xFF usually means no legacy ATA device responds here.
     */
    if (status == 0x00 || status == 0xFF) {
        return 0;
    }

    if (!ata_wait_not_busy()) {
        return 0;
    }

    status = ata_read_status();

    if (status & ATA_SR_ERR) {
        return 0;
    }

    if (!ata_wait_drq_or_error()) {
        return 0;
    }

    for (nq_u32 i = 0; i < 256; i++) {
        identify[i] = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
    }

    /*
     * Words 60-61 = total LBA28 sectors.
     */
    g_sector_count =
        ((nq_u64)identify[60]) |
        ((nq_u64)identify[61] << 16);

    if (g_sector_count == 0) {
        /*
         * Keep it usable for simple QEMU disks even if identify is odd.
         */
        g_sector_count = 0x0FFFFFFFull;
    }

    g_block_ready = 1;
    return 1;
}

static int ata_read_one_sector_lba28(nq_u32 lba, void* buffer) {
    if (!buffer) {
        return 0;
    }

    if (!ata_wait_not_busy()) {
        return 0;
    }

    ata_select_primary_master_lba28(lba);

    outb(ATA_PRIMARY_IO + ATA_REG_FEATURES, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (nq_u8)(lba & 0xFFu));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (nq_u8)((lba >> 8) & 0xFFu));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (nq_u8)((lba >> 16) & 0xFFu));

    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (!ata_wait_drq_or_error()) {
        return 0;
    }

    nq_u8* out = (nq_u8*)buffer;

    for (nq_u32 i = 0; i < 256; i++) {
        nq_u16 word = inw(ATA_PRIMARY_IO + ATA_REG_DATA);

        out[i * 2] = (nq_u8)(word & 0xFFu);
        out[i * 2 + 1] = (nq_u8)((word >> 8) & 0xFFu);
    }

    ata_400ns_delay();

    return 1;
}

static int ata_write_one_sector_lba28(nq_u32 lba, const void* buffer) {
    if (!buffer) {
        return 0;
    }

    if (!ata_wait_not_busy()) {
        return 0;
    }

    ata_select_primary_master_lba28(lba);

    outb(ATA_PRIMARY_IO + ATA_REG_FEATURES, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (nq_u8)(lba & 0xFFu));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (nq_u8)((lba >> 8) & 0xFFu));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (nq_u8)((lba >> 16) & 0xFFu));

    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (!ata_wait_drq_or_error()) {
        return 0;
    }

    const nq_u8* in = (const nq_u8*)buffer;

    for (nq_u32 i = 0; i < 256; i++) {
        nq_u16 word =
            ((nq_u16)in[i * 2]) |
            ((nq_u16)in[i * 2 + 1] << 8);

        outw(ATA_PRIMARY_IO + ATA_REG_DATA, word);
    }

    /*
     * Finish the data transfer before issuing cache flush.
     */
    if (!ata_wait_not_busy()) {
        return 0;
    }

    if (!ata_status_ok()) {
        return 0;
    }

    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);

    if (!ata_wait_not_busy()) {
        return 0;
    }

    if (!ata_status_ok()) {
        return 0;
    }

    ata_400ns_delay();

    return 1;
}

/* ============================================================
 * Public block64 API
 * ============================================================ */

int block64_init(void) {
    block64_clear_devices();

    if (!ata_identify_primary_master()) {
        g_block_ready = 0;
        g_sector_count = 0;
        return 0;
    }

    /*
     * In SAFE_HW, if block64_init() is ever called, register the device
     * as read-only. main.c should normally skip block64_init() entirely
     * in SAFE_HW mode, but this keeps block64 safe by itself too.
     */
    int read_only = safe_mode64_enabled() ? 1 : 0;

    block64_device_t* dev = block64_register_device(
        "ide0",
        BLOCK64_DEVICE_ATA_PIO,
        g_sector_count,
        read_only
    );

    if (!dev) {
        g_block_ready = 0;
        return 0;
    }

    g_block_ready = 1;
    return 1;
}

int block64_available(void) {
    return g_block_ready;
}

int block64_ready(void) {
    return g_block_ready;
}

nq_u64 block64_sector_count(void) {
    return g_sector_count;
}

const block64_device_t* block64_boot_device(void) {
    return g_boot_device;
}

const block64_device_t* block64_get_device(nq_u32 id) {
    if (id >= g_block_device_count) {
        return 0;
    }

    if (!g_block_devices[id].present) {
        return 0;
    }

    return &g_block_devices[id];
}

nq_u32 block64_device_count(void) {
    return g_block_device_count;
}

const char* block64_device_type_name(block64_device_type_t type) {
    switch (type) {
        case BLOCK64_DEVICE_ATA_PIO:
            return "ATA_PIO";

        case BLOCK64_DEVICE_AHCI:
            return "AHCI";

        case BLOCK64_DEVICE_USB_MSC:
            return "USB_MSC";

        case BLOCK64_DEVICE_NVME:
            return "NVME";

        case BLOCK64_DEVICE_RAMDISK:
            return "RAMDISK";

        case BLOCK64_DEVICE_NONE:
        default:
            return "NONE";
    }
}

void block64_print_devices(void) {
    k64_print("Block64 devices:\n");

    if (g_block_device_count == 0) {
        k64_print("  none\n");
        return;
    }

    k64_print("ID  NAME   TYPE      SECTORS       MODE  WRITE\n");

    for (nq_u32 i = 0; i < g_block_device_count; i++) {
        block64_device_t* dev = &g_block_devices[i];

        if (!dev->present) {
            continue;
        }

        k64_print_dec64(dev->id);
        k64_print("   ");

        k64_print(dev->name);
        k64_print("   ");

        k64_print(block64_device_type_name(dev->type));
        k64_print("   ");

        k64_print_dec64(dev->sector_count);
        k64_print("   ");

        k64_print(dev->read_only ? "RO" : "RW");
        k64_print("    ");

        if (dev->read_only) {
            k64_print("blocked-readonly");
        } else if (dev->writes_unlocked) {
            k64_print("unlocked");
        } else {
            k64_print("locked");
        }

        if (dev == g_boot_device) {
            k64_print("   boot");
        }

        k64_print("\n");
    }
}

int block64_unlock_writes(nq_u32 device_id) {
    if (device_id >= g_block_device_count) {
        return 0;
    }

    block64_device_t* dev = &g_block_devices[device_id];

    if (!dev->present || dev->read_only) {
        return 0;
    }

    dev->writes_unlocked = 1;
    return 1;
}

int block64_lock_writes(nq_u32 device_id) {
    if (device_id >= g_block_device_count) {
        return 0;
    }

    block64_device_t* dev = &g_block_devices[device_id];

    if (!dev->present) {
        return 0;
    }

    dev->writes_unlocked = 0;
    return 1;
}

int block64_read(nq_u64 lba, void* buffer, nq_u32 sector_count) {
    if (!g_boot_device) {
        return 0;
    }

    return block64_read_device(
        g_boot_device->id,
        lba,
        buffer,
        sector_count
    );
}

int block64_read_device(
    nq_u32 device_id,
    nq_u64 lba,
    void* buffer,
    nq_u32 sector_count
) {
    const block64_device_t* dev = block64_get_device(device_id);

    if (!dev || !g_block_ready || !buffer) {
        return 0;
    }

    if (sector_count == 0) {
        return 1;
    }

    if (dev->sector_size != BLOCK64_SECTOR_SIZE) {
        return 0;
    }

    if (dev->sector_count != 0 &&
        (lba + sector_count) > dev->sector_count) {
        return 0;
    }

    /*
     * Early version: LBA28 only.
     */
    if (lba > 0x0FFFFFFFull) {
        return 0;
    }

    if ((lba + sector_count) > 0x10000000ull) {
        return 0;
    }

    switch (dev->type) {
        case BLOCK64_DEVICE_ATA_PIO:
            break;

        default:
            return 0;
    }

    nq_u8* out = (nq_u8*)buffer;

    for (nq_u32 i = 0; i < sector_count; i++) {
        if (!ata_read_one_sector_lba28(
                (nq_u32)(lba + i),
                out + ((nq_u64)i * BLOCK64_SECTOR_SIZE))) {
            return 0;
        }
    }

    return 1;
}

int block64_write(nq_u64 lba, const void* buffer, nq_u32 sector_count) {
    if (!g_boot_device) {
        return 0;
    }

    return block64_write_device(
        g_boot_device->id,
        lba,
        buffer,
        sector_count
    );
}

int block64_write_device(
    nq_u32 device_id,
    nq_u64 lba,
    const void* buffer,
    nq_u32 sector_count
) {
    if (device_id >= g_block_device_count) {
        return 0;
    }

    block64_device_t* dev = &g_block_devices[device_id];

    if (!dev->present || !g_block_ready || !buffer) {
        return 0;
    }

    if (sector_count == 0) {
        return 1;
    }

    if (dev->sector_size != BLOCK64_SECTOR_SIZE) {
        return 0;
    }

    if (dev->read_only) {
        k64_print("block64: write blocked, device is read-only: ");
        k64_print(dev->name);
        k64_print("\n");
        return 0;
    }

    if (!dev->writes_unlocked) {
        k64_print("block64: write blocked, device writes locked: ");
        k64_print(dev->name);
        k64_print("\n");
        return 0;
    }

    if (!safe_mode64_disk_writes_allowed()) {
        k64_print("block64: write blocked by SAFE_HW mode\n");
        return 0;
    }

    if (dev->sector_count != 0 &&
        (lba + sector_count) > dev->sector_count) {
        return 0;
    }

    /*
     * Early version: LBA28 only.
     */
    if (lba > 0x0FFFFFFFull) {
        return 0;
    }

    if ((lba + sector_count) > 0x10000000ull) {
        return 0;
    }

    switch (dev->type) {
        case BLOCK64_DEVICE_ATA_PIO:
            break;

        default:
            return 0;
    }

    const nq_u8* in = (const nq_u8*)buffer;

    for (nq_u32 i = 0; i < sector_count; i++) {
        if (!ata_write_one_sector_lba28(
                (nq_u32)(lba + i),
                in + ((nq_u64)i * BLOCK64_SECTOR_SIZE))) {
            return 0;
        }
    }

    return 1;
}

void block64_print_summary(void) {
    k64_print("Block64 storage: ");

    if (!g_block_ready) {
        k64_print("not available\n");
        return;
    }

    k64_print("available\n");

    k64_print("Sector size: ");
    k64_print_dec64(BLOCK64_SECTOR_SIZE);
    k64_print(" bytes\n");

    k64_print("Sector count: ");
    k64_print_dec64(g_sector_count);
    k64_print("\n");

    k64_print("Capacity: ");
    k64_print_dec64((g_sector_count * BLOCK64_SECTOR_SIZE) / 1024ull);
    k64_print(" KiB\n");

    block64_print_devices();
}