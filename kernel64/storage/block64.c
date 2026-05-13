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

    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);

    if (!ata_wait_not_busy()) {
        return 0;
    }

    ata_400ns_delay();

    return 1;
}

int block64_init(void) {
    if (!ata_identify_primary_master()) {
        g_block_ready = 0;
        g_sector_count = 0;
        return 0;
    }

    return 1;
}

int block64_available(void) {
    return g_block_ready;
}

int block64_read(nq_u64 lba, void* buffer, nq_u32 sector_count) {
    if (!g_block_ready || !buffer) {
        return 0;
    }

    if (sector_count == 0) {
        return 1;
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
    if (!safe_mode64_disk_writes_allowed()) {
        k64_print("block64: write blocked by SAFE_HW mode\n");
        return 0;
    }

    if (!g_block_ready || !buffer) {
        return 0;
    }

    if (sector_count == 0) {
        return 1;
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

// int block64_write(nq_u64 lba, const void* buffer, nq_u32 sector_count) {
//     (void)lba;
//     (void)buffer;
//     (void)sector_count;
//     return 0;
// }

nq_u64 block64_sector_count(void) {
    return g_sector_count;
}

void block64_print_summary(void) {
    k64_print("Block64 storage: ");

    if (!g_block_ready) {
        k64_print("not available\n");
        return;
    }

    k64_print("primary ATA disk available\n");

    k64_print("Sector size: ");
    k64_print_dec64(BLOCK64_SECTOR_SIZE);
    k64_print(" bytes\n");

    k64_print("Sector count: ");
    k64_print_dec64(g_sector_count);
    k64_print("\n");

    k64_print("Capacity: ");
    k64_print_dec64((g_sector_count * BLOCK64_SECTOR_SIZE) / 1024ull);
    k64_print(" KiB\n");
}
