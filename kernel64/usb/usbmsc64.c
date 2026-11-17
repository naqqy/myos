#include "usbmsc64.h"
#include "usb64.h"

#include "../k64_console.h"
#include "../mm/pmm64.h"
#include "../proc/process64.h"
#include "../storage/block64.h"
#include "../fs/vfs64.h"

/*
 * USB mass storage, bulk-only transport. See usbmsc64.h.
 *
 * Every operation is three bulk transfers:
 *
 *   1. host -> device: a 31-byte "command block wrapper" (CBW) carrying a
 *      SCSI command such as READ(10),
 *   2. the data, in whichever direction the command says,
 *   3. device -> host: a 13-byte "command status wrapper" (CSW) saying
 *      whether it worked.
 *
 * The driver can be entered from any task (see the locking rule in
 * xhci64.h), so the public functions take the task-switch guard.
 */

#define SECTOR_SIZE 512u

/*
 * Largest single transfer, and the size of the read-ahead cache.
 */
#define CHUNK_SECTORS 64u
#define CHUNK_BYTES   (CHUNK_SECTORS * SECTOR_SIZE)

#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u

#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE   0x03u
#define SCSI_READ_CAPACITY   0x25u
#define SCSI_READ_10         0x28u
#define SCSI_WRITE_10        0x2Au

typedef struct usbmsc {
    int present;

    xhci64_t* hc;
    nq_u8 slot;
    nq_u8 interface;
    nq_u8 ep_in;
    nq_u8 ep_out;

    nq_u64 sectors;
    nq_u32 tag;

    /*
     * Memory the controller transfers to and from: one page for the
     * command and status wrappers, and a 32 KiB window for data.
     */
    nq_u8* wrappers;
    nq_u8* data;

    /*
     * The data window doubles as a read-ahead cache: after a read it
     * holds cache_count sectors starting at cache_lba.
     */
    nq_u64 cache_lba;
    nq_u32 cache_count;

    int mounted;

    nq_u64 reads;
    nq_u64 writes;
    nq_u64 errors;
} usbmsc_t;

static usbmsc_t g_disk;

static nq_u8 g_sector[SECTOR_SIZE];
static nq_u8 g_table[SECTOR_SIZE];

static void copy_bytes(void* dst, const void* src, nq_u64 count) {
    nq_u8* d = (nq_u8*)dst;
    const nq_u8* s = (const nq_u8*)src;

    for (nq_u64 i = 0; i < count; i++) {
        d[i] = s[i];
    }
}

static void put_le32(nq_u8* p, nq_u32 v) {
    p[0] = (nq_u8)v;
    p[1] = (nq_u8)(v >> 8);
    p[2] = (nq_u8)(v >> 16);
    p[3] = (nq_u8)(v >> 24);
}

static nq_u32 get_le32(const nq_u8* p) {
    return (nq_u32)p[0] | ((nq_u32)p[1] << 8) | ((nq_u32)p[2] << 16) | ((nq_u32)p[3] << 24);
}

static nq_u64 get_le64(const nq_u8* p) {
    return (nq_u64)get_le32(p) | ((nq_u64)get_le32(p + 4) << 32);
}

static nq_u32 get_be32(const nq_u8* p) {
    return ((nq_u32)p[0] << 24) | ((nq_u32)p[1] << 16) | ((nq_u32)p[2] << 8) | (nq_u32)p[3];
}

/*
 * ------------------------------------------------------------
 * Transport
 * ------------------------------------------------------------
 */

static void clear_halt(nq_u8 endpoint) {
    /*
     * CLEAR_FEATURE(ENDPOINT_HALT), addressed to the endpoint.
     */
    xhci64_control(g_disk.hc, g_disk.slot, 0x02u, 1u, 0, endpoint, 0, 0);
}

/*
 * The protocol's "something went badly wrong" procedure: class reset,
 * then un-halt both endpoints.
 */
static void reset_recovery(void) {
    xhci64_control(g_disk.hc, g_disk.slot, 0x21u, 0xFFu, 0, g_disk.interface, 0, 0);

    clear_halt(g_disk.ep_in);
    clear_halt(g_disk.ep_out);
}

/*
 * Runs one SCSI command. Data goes to or comes from g_disk.data.
 * Returns 1 if the device reported success.
 */
static int scsi_command(const nq_u8* cdb, nq_u32 cdb_length, int data_in, nq_u32 data_length) {
    nq_u8* cbw = g_disk.wrappers;
    nq_u8* csw = g_disk.wrappers + 64u;
    nq_u32 moved = 0;
    int result;

    for (nq_u32 i = 0; i < 31u; i++) {
        cbw[i] = 0;
    }

    g_disk.tag++;

    put_le32(cbw + 0, CBW_SIGNATURE);
    put_le32(cbw + 4, g_disk.tag);
    put_le32(cbw + 8, data_length);
    cbw[12] = data_in ? 0x80u : 0x00u;
    cbw[13] = 0;
    cbw[14] = (nq_u8)cdb_length;
    copy_bytes(cbw + 15, cdb, cdb_length);

    result = xhci64_bulk(g_disk.hc, g_disk.slot, g_disk.ep_out, cbw, 31u, &moved);

    if (result != 1 || moved != 31u) {
        reset_recovery();
        return 0;
    }

    if (data_length) {
        nq_u8 endpoint = data_in ? g_disk.ep_in : g_disk.ep_out;

        result = xhci64_bulk(g_disk.hc, g_disk.slot, endpoint, g_disk.data, data_length, &moved);

        if (result == -1) {
            /*
             * A stall in the data stage is how a device refuses a command;
             * the status still follows.
             */
            clear_halt(endpoint);
        } else if (result == 0) {
            reset_recovery();
            return 0;
        }
    }

    result = xhci64_bulk(g_disk.hc, g_disk.slot, g_disk.ep_in, csw, 13u, &moved);

    if (result == -1) {
        clear_halt(g_disk.ep_in);
        result = xhci64_bulk(g_disk.hc, g_disk.slot, g_disk.ep_in, csw, 13u, &moved);
    }

    if (result != 1 || moved != 13u ||
        get_le32(csw + 0) != CSW_SIGNATURE ||
        get_le32(csw + 4) != g_disk.tag ||
        csw[12] > 1u) {
        reset_recovery();
        return 0;
    }

    return csw[12] == 0;
}

static void request_sense(void) {
    nq_u8 cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };

    scsi_command(cdb, 6, 1, 18);
}

static int transfer_sectors(int write, nq_u64 lba, nq_u32 count) {
    nq_u8 cdb[10];

    cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
    cdb[1] = 0;
    cdb[2] = (nq_u8)(lba >> 24);
    cdb[3] = (nq_u8)(lba >> 16);
    cdb[4] = (nq_u8)(lba >> 8);
    cdb[5] = (nq_u8)lba;
    cdb[6] = 0;
    cdb[7] = (nq_u8)(count >> 8);
    cdb[8] = (nq_u8)count;
    cdb[9] = 0;

    for (nq_u32 attempt = 0; attempt < 3u; attempt++) {
        if (scsi_command(cdb, 10, !write, count * SECTOR_SIZE)) {
            return 1;
        }

        g_disk.errors++;

        if (!g_disk.present) {
            break;
        }

        request_sense();
    }

    return 0;
}

/*
 * ------------------------------------------------------------
 * Sector I/O
 * ------------------------------------------------------------
 */

static int read_locked(nq_u64 lba, nq_u8* out, nq_u32 count) {
    while (count) {
        if (!g_disk.present || lba >= g_disk.sectors) {
            return 0;
        }

        if (g_disk.cache_count &&
            lba >= g_disk.cache_lba &&
            lba < g_disk.cache_lba + g_disk.cache_count) {
            nq_u64 offset = lba - g_disk.cache_lba;
            nq_u32 available = g_disk.cache_count - (nq_u32)offset;
            nq_u32 take = count < available ? count : available;

            copy_bytes(out, g_disk.data + offset * SECTOR_SIZE, (nq_u64)take * SECTOR_SIZE);

            out += (nq_u64)take * SECTOR_SIZE;
            lba += take;
            count -= take;
            continue;
        }

        /*
         * Miss: fetch a whole chunk starting here. Files are mostly read
         * front to back, so the following sectors are likely next.
         */
        nq_u64 remaining = g_disk.sectors - lba;
        nq_u32 fetch = remaining < CHUNK_SECTORS ? (nq_u32)remaining : CHUNK_SECTORS;

        g_disk.cache_count = 0;

        if (!transfer_sectors(0, lba, fetch)) {
            return 0;
        }

        g_disk.cache_lba = lba;
        g_disk.cache_count = fetch;
        g_disk.reads++;
    }

    return 1;
}

static int write_locked(nq_u64 lba, const nq_u8* in, nq_u32 count) {
    while (count) {
        if (!g_disk.present || lba >= g_disk.sectors) {
            return 0;
        }

        nq_u64 remaining = g_disk.sectors - lba;
        nq_u32 chunk = count < CHUNK_SECTORS ? count : CHUNK_SECTORS;

        if (chunk > remaining) {
            return 0;
        }

        /*
         * The data window is about to be overwritten: the cache is gone.
         */
        g_disk.cache_count = 0;

        copy_bytes(g_disk.data, in, (nq_u64)chunk * SECTOR_SIZE);

        if (!transfer_sectors(1, lba, chunk)) {
            return 0;
        }

        g_disk.writes++;

        in += (nq_u64)chunk * SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }

    return 1;
}

int usbmsc64_read(nq_u64 lba, void* buffer, nq_u32 sector_count) {
    if (!buffer) {
        return 0;
    }

    process64_preempt_disable();

    int ok = read_locked(lba, (nq_u8*)buffer, sector_count);

    process64_preempt_enable();

    return ok;
}

int usbmsc64_write(nq_u64 lba, const void* buffer, nq_u32 sector_count) {
    if (!buffer) {
        return 0;
    }

    process64_preempt_disable();

    int ok = write_locked(lba, (const nq_u8*)buffer, sector_count);

    process64_preempt_enable();

    return ok;
}

/*
 * ------------------------------------------------------------
 * Finding the filesystem
 * ------------------------------------------------------------
 */

static int is_fat32_boot_sector(const nq_u8* s) {
    return s[510] == 0x55u && s[511] == 0xAAu &&
           s[82] == 'F' && s[83] == 'A' && s[84] == 'T' && s[85] == '3' && s[86] == '2';
}

static int sector_is_fat32(nq_u64 lba) {
    return lba < g_disk.sectors &&
           read_locked(lba, g_sector, 1) &&
           is_fat32_boot_sector(g_sector);
}

/*
 * A stick is either one big filesystem starting at sector 0, or has a
 * partition table (old-style MBR or GPT) pointing at it. Returns 1 and
 * the first sector of the first FAT32 filesystem found.
 */
static int find_fat32(nq_u64* out_lba) {
    nq_u8* table = g_table;

    if (!read_locked(0, table, 1)) {
        return 0;
    }

    if (is_fat32_boot_sector(table)) {
        *out_lba = 0;
        return 1;
    }

    if (table[510] != 0x55u || table[511] != 0xAAu) {
        return 0;
    }

    int is_gpt = 0;

    for (nq_u32 i = 0; i < 4u; i++) {
        const nq_u8* entry = table + 446u + i * 16u;
        nq_u8 type = entry[4];
        nq_u32 start = get_le32(entry + 8);

        if (type == 0xEEu) {
            is_gpt = 1;
            continue;
        }

        if (type == 0 || start == 0) {
            continue;
        }

        if (sector_is_fat32(start)) {
            *out_lba = start;
            return 1;
        }
    }

    if (!is_gpt || !read_locked(1, table, 1)) {
        return 0;
    }

    /*
     * GPT header: "EFI PART", then (among other things) where the entry
     * array starts, how many entries it has and how big each one is.
     */
    if (table[0] != 'E' || table[1] != 'F' || table[2] != 'I' || table[3] != ' ') {
        return 0;
    }

    nq_u64 entries_lba = get_le64(table + 72);
    nq_u32 entry_count = get_le32(table + 80);
    nq_u32 entry_size = get_le32(table + 84);

    if (entry_size < 128u || entry_size > SECTOR_SIZE) {
        return 0;
    }

    if (entry_count > 32u) {
        entry_count = 32u;
    }

    nq_u32 per_sector = SECTOR_SIZE / entry_size;

    for (nq_u32 i = 0; i < entry_count; i++) {
        if (!read_locked(entries_lba + i / per_sector, table, 1)) {
            return 0;
        }

        const nq_u8* entry = table + (i % per_sector) * entry_size;
        nq_u64 first = get_le64(entry + 32);

        if (first == 0) {
            continue;
        }

        if (sector_is_fat32(first)) {
            *out_lba = first;
            return 1;
        }
    }

    return 0;
}

/*
 * ------------------------------------------------------------
 * Attach / detach
 * ------------------------------------------------------------
 */

/*
 * A transfer buffer must not cross a 64 KiB boundary. Any 64 KiB of
 * memory contains a 32 KiB stretch that does not.
 */
static nq_u8* pick_window(nq_u8* base) {
    nq_u64 start = (nq_u64)base;
    nq_u64 boundary = (start + 0xFFFFull) & ~0xFFFFull;

    if (boundary - start >= CHUNK_BYTES) {
        return base;
    }

    return (nq_u8*)boundary;
}

int usbmsc64_attach(xhci64_t* hc, nq_u8 slot, const usbmsc64_interface_t* intf) {
    if (!hc || !intf || !intf->ep_in || !intf->ep_out) {
        return 0;
    }

    if (g_disk.present) {
        usb64_log("USB: only one USB disk is supported at a time; ignoring this one\n");
        return 0;
    }

    if (!g_disk.wrappers) {
        g_disk.wrappers = (nq_u8*)pmm64_alloc_frame();

        nq_u8* block = (nq_u8*)pmm64_alloc_contiguous_frames(16);

        if (!g_disk.wrappers || !block) {
            usb64_log("USB: no memory for the disk buffers\n");
            return 0;
        }

        g_disk.data = pick_window(block);
    }

    g_disk.hc = hc;
    g_disk.slot = slot;
    g_disk.interface = intf->number;
    g_disk.ep_in = intf->ep_in;
    g_disk.ep_out = intf->ep_out;
    g_disk.sectors = 0;
    g_disk.cache_count = 0;
    g_disk.mounted = 0;
    g_disk.reads = 0;
    g_disk.writes = 0;
    g_disk.errors = 0;
    g_disk.present = 1;

    /*
     * A freshly plugged disk answers "not ready" for a while (a card
     * reader with no card answers that forever).
     */
    nq_u8 ready_cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    int ready = 0;

    for (nq_u32 i = 0; i < 20u && !ready; i++) {
        if (scsi_command(ready_cdb, 6, 0, 0)) {
            ready = 1;
        } else {
            request_sense();
            xhci64_idle_ms(100);
        }
    }

    nq_u8 capacity_cdb[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (!ready || !scsi_command(capacity_cdb, 10, 1, 8)) {
        usb64_log("USB: disk is not ready (no media?)\n");
        g_disk.present = 0;
        return 0;
    }

    nq_u32 last_lba = get_be32(g_disk.data);
    nq_u32 block_size = get_be32(g_disk.data + 4);

    if (block_size != SECTOR_SIZE) {
        usb64_log("USB: disk uses ");
        usb64_log_dec(block_size);
        usb64_log("-byte sectors, only 512 is supported\n");
        g_disk.present = 0;
        return 0;
    }

    g_disk.sectors = (nq_u64)last_lba + 1u;

    usb64_log("USB: disk, ");
    usb64_log_dec(g_disk.sectors / 2048u);
    usb64_log(" MiB");

    int block_id = block64_usb_attach(g_disk.sectors);
    nq_u64 fat_lba = 0;

    if (block_id < 0) {
        usb64_log(", no free block device slot\n");
    } else if (!find_fat32(&fat_lba)) {
        usb64_log(", no FAT32 filesystem found\n");
    } else if (vfs64_usb_mount((nq_u32)block_id, fat_lba)) {
        g_disk.mounted = 1;
        usb64_log(", mounted at /usb");

        /*
         * The volume serial sits at offset 67 of the FAT32 boot sector.
         * If it is the one the loader started from, this is our own boot
         * stick and may be written to.
         */
        if (read_locked(fat_lba, g_sector, 1) &&
            block64_note_volume_serial((nq_u32)block_id, get_le32(g_sector + 67))) {
            usb64_log(" (boot stick, writable)\n");
        } else {
            usb64_log(" (writes locked)\n");
        }
    } else {
        usb64_log(", FAT32 found but not mounted\n");
    }

    return 1;
}

void usbmsc64_detach(xhci64_t* hc, nq_u8 slot) {
    if (!g_disk.present || g_disk.hc != hc || g_disk.slot != slot) {
        return;
    }

    /*
     * Mark it gone first, so I/O from other tasks fails instead of
     * talking to a device that is being torn down.
     */
    g_disk.present = 0;
    g_disk.cache_count = 0;

    if (g_disk.mounted) {
        vfs64_usb_unmount();
        g_disk.mounted = 0;
    }

    block64_usb_detach();
}

void usbmsc64_print_status(void) {
    if (!g_disk.present) {
        return;
    }

    k64_print("  disk: ");
    k64_print_dec64(g_disk.sectors / 2048u);
    k64_print(" MiB, ");
    k64_print(g_disk.mounted ? "mounted at /usb" : "not mounted");
    k64_print(", ");
    k64_print_dec64(g_disk.reads);
    k64_print(" reads, ");
    k64_print_dec64(g_disk.writes);
    k64_print(" writes, ");
    k64_print_dec64(g_disk.errors);
    k64_print(" errors\n");
}
