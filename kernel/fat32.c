#include "fat32.h"

extern void kernel_print(const char* s);
extern void kernel_put_char(char c);

static void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void outw(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_wait(void) {
    outb(0x80, 0);
}

#define ATA_DATA       0x1F0
#define ATA_SECCOUNT0  0x1F2
#define ATA_LBA0       0x1F3
#define ATA_LBA1       0x1F4
#define ATA_LBA2       0x1F5
#define ATA_HDDEVSEL   0x1F6
#define ATA_COMMAND    0x1F7
#define ATA_STATUS     0x1F7
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_CMD_READ_PIO 0x20

#define FAT_FS_START_LBA 65

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

#define FAT32_EOC          0x0FFFFFF8u
#define FAT32_BAD_CLUSTER  0x0FFFFFF7u
#define FAT32_CLUSTER_MASK 0x0FFFFFFFu

#define FAT_MAX_LONG_NAME  128
#define FAT_MAX_COMPONENT  128

typedef struct __attribute__((packed)) {
    u8  jump[3];
    u8  oem[8];

    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  num_fats;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8  media;
    u16 sectors_per_fat_16;
    u16 sectors_per_track;
    u16 num_heads;
    u32 hidden_sectors;
    u32 total_sectors_32;

    u32 sectors_per_fat_32;
    u16 ext_flags;
    u16 fs_version;
    u32 root_cluster;
    u16 fs_info;
    u16 backup_boot_sector;
    u8  reserved[12];
    u8  drive_number;
    u8  reserved1;
    u8  boot_signature;
    u32 volume_id;
    u8  volume_label[11];
    u8  fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    u8  name[11];
    u8  attr;
    u8  nt_reserved;
    u8  create_time_tenths;
    u16 create_time;
    u16 create_date;
    u16 last_access_date;
    u16 first_cluster_high;
    u16 write_time;
    u16 write_date;
    u16 first_cluster_low;
    u32 file_size;
} fat_dir_entry_t;

typedef struct {
    char long_name[FAT_MAX_LONG_NAME];
    char short_name[13];
    u8 attr;
    u32 first_cluster;
    u32 file_size;
    int is_directory;
} fat_found_entry_t;

static fat32_bpb_t bpb;

static u32 fat_start_lba = 0;
static u32 data_start_lba = 0;
static u32 root_cluster = 0;
static u32 sectors_per_fat = 0;

static u32 total_sectors = 0;

static int fat_mounted = 0;

static u8 sector_buf[512];

static int ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        u8 status = inb(ATA_STATUS);

        if (!(status & 0x80) && (status & 0x08)) {
            return 1;
        }
    }

    return 0;
}

static int ata_wait_not_busy(void) {
    for (int i = 0; i < 100000; i++) {
        u8 status = inb(ATA_STATUS);

        if (!(status & 0x80)) {
            return 1;
        }
    }

    return 0;
}

static int ata_write_sector(u32 lba, const u8* buffer) {
    outb(ATA_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCOUNT0, 1);
    outb(ATA_LBA0, (u8)(lba & 0xFF));
    outb(ATA_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);

    if (!ata_wait_ready()) {
        return 0;
    }

    for (int i = 0; i < 256; i++) {
        u16 word = (u16)buffer[i * 2] | ((u16)buffer[i * 2 + 1] << 8);
        outw(ATA_DATA, word);
    }

    io_wait();

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);

    if (!ata_wait_not_busy()) {
        return 0;
    }

    return 1;
}

static int ata_read_sector(u32 lba, u8* buffer) {
    outb(ATA_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCOUNT0, 1);
    outb(ATA_LBA0, (u8)(lba & 0xFF));
    outb(ATA_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);

    if (!ata_wait_ready()) {
        return 0;
    }

    for (int i = 0; i < 256; i++) {
        u16 word;

        __asm__ volatile ("inw %1, %0" : "=a"(word) : "Nd"(ATA_DATA));

        buffer[i * 2]     = (u8)(word & 0xFF);
        buffer[i * 2 + 1] = (u8)(word >> 8);
    }

    io_wait();
    return 1;
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + 32);
    }

    return c;
}

static int str_eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return 0;
        }

        a++;
        b++;
    }

    return *a == 0 && *b == 0;
}

static void str_clear(char* s, u32 max) {
    for (u32 i = 0; i < max; i++) {
        s[i] = 0;
    }
}

static int entry_is_dot_name(const fat_dir_entry_t* e) {
    return e->name[0] == '.' &&
           (e->name[1] == ' ' || e->name[1] == '.');
}

static int entry_short_name_empty(const fat_dir_entry_t* e) {
    for (int i = 0; i < 11; i++) {
        if (e->name[i] != ' ') {
            return 0;
        }
    }

    return 1;
}

static void str_copy_safe(char* dst, const char* src, u32 max) {
    u32 i = 0;

    if (max == 0) {
        return;
    }

    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static u32 first_cluster_of_entry(const fat_dir_entry_t* e) {
    u32 high = (u32)e->first_cluster_high;
    u32 low  = (u32)e->first_cluster_low;

    return (high << 16) | low;
}

static int cluster_is_eoc(u32 cluster) {
    return cluster >= FAT32_EOC;
}

static int cluster_is_valid(u32 cluster) {
    if (cluster < 2) {
        return 0;
    }

    if (cluster == FAT32_BAD_CLUSTER) {
        return 0;
    }

    if (cluster_is_eoc(cluster)) {
        return 0;
    }

    return 1;
}

static u32 cluster_to_lba(u32 cluster) {
    return data_start_lba + ((cluster - 2) * (u32)bpb.sectors_per_cluster);
}

static u32 fat32_next_cluster(u32 cluster) {
    u32 fat_offset = cluster * 4;
    u32 fat_sector = fat_start_lba + (fat_offset / bpb.bytes_per_sector);
    u32 ent_offset = fat_offset % bpb.bytes_per_sector;

    if (!ata_read_sector(fat_sector, sector_buf)) {
        return FAT32_EOC;
    }

    u32 value =
        ((u32)sector_buf[ent_offset]) |
        ((u32)sector_buf[ent_offset + 1] << 8) |
        ((u32)sector_buf[ent_offset + 2] << 16) |
        ((u32)sector_buf[ent_offset + 3] << 24);

    return value & FAT32_CLUSTER_MASK;
}

static int entry_is_lfn(const fat_dir_entry_t* e) {
    return e->attr == FAT_ATTR_LFN;
}

static int entry_is_skippable(const fat_dir_entry_t* e) {
    if (e->name[0] == 0x00) {
        return 1;
    }

    if (e->name[0] == 0xE5) {
        return 1;
    }

    if (entry_is_lfn(e)) {
        return 1;
    }

    if (e->attr & FAT_ATTR_VOLUME_ID) {
        return 1;
    }

    if (entry_is_dot_name(e)) {
        return 1;
    }

    if (entry_short_name_empty(e)) {
        return 1;
    }

    return 0;
}

static void make_short_name_string(const fat_dir_entry_t* e, char out[13]) {
    int pos = 0;
    int has_ext = 0;

    for (int i = 0; i < 13; i++) {
        out[i] = 0;
    }

    for (int i = 0; i < 8; i++) {
        if (e->name[i] != ' ') {
            out[pos++] = (char)e->name[i];
        }
    }

    for (int i = 8; i < 11; i++) {
        if (e->name[i] != ' ') {
            has_ext = 1;
        }
    }

    if (has_ext) {
        out[pos++] = '.';

        for (int i = 8; i < 11; i++) {
            if (e->name[i] != ' ') {
                out[pos++] = (char)e->name[i];
            }
        }
    }

    out[pos] = 0;
}

static void lfn_put_char(char* lfn, u32 index, u16 ch) {
    if (index >= FAT_MAX_LONG_NAME - 1) {
        return;
    }

    if (ch == 0x0000) {
        lfn[index] = 0;
        return;
    }

    if (ch == 0xFFFF) {
        return;
    }

    /*
     * VFAT stores UTF-16. For now we support ASCII subset cleanly.
     * Non-ASCII becomes '?'.
     */
    if (ch < 128) {
        lfn[index] = (char)ch;
    } else {
        lfn[index] = '?';
    }
}

static u16 lfn_u16_at(const u8* p) {
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static void collect_lfn_entry(const u8* raw, char* lfn) {
    u8 order = raw[0];
    u32 seq = order & 0x1F;

    if (seq == 0) {
        return;
    }

    if (order & 0x40) {
        str_clear(lfn, FAT_MAX_LONG_NAME);
    }

    u32 base = (seq - 1) * 13;
    u32 out = base;

    for (int i = 0; i < 5; i++) {
        lfn_put_char(lfn, out++, lfn_u16_at(raw + 1 + i * 2));
    }

    for (int i = 0; i < 6; i++) {
        lfn_put_char(lfn, out++, lfn_u16_at(raw + 14 + i * 2));
    }

    for (int i = 0; i < 2; i++) {
        lfn_put_char(lfn, out++, lfn_u16_at(raw + 28 + i * 2));
    }
}

static int component_matches_entry(
    const char* component,
    const char* long_name,
    const char* short_name
) {
    if (long_name && long_name[0] && str_eq_ci(component, long_name)) {
        return 1;
    }

    if (short_name && short_name[0] && str_eq_ci(component, short_name)) {
        return 1;
    }

    return 0;
}

static void fill_found_entry(
    fat_found_entry_t* out,
    const fat_dir_entry_t* e,
    const char* lfn
) {
    char short_name[13];

    make_short_name_string(e, short_name);

    str_clear(out->long_name, sizeof(out->long_name));
    str_clear(out->short_name, sizeof(out->short_name));

    if (lfn && lfn[0]) {
        str_copy_safe(out->long_name, lfn, sizeof(out->long_name));
    }

    str_copy_safe(out->short_name, short_name, sizeof(out->short_name));

    out->attr = e->attr;
    out->first_cluster = first_cluster_of_entry(e);
    out->file_size = e->file_size;
    out->is_directory = (e->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
}

static int next_path_component(const char** path_ptr, char* out, u32 out_size) {
    const char* p = *path_ptr;
    u32 i = 0;

    while (*p == '/' || *p == '\\') {
        p++;
    }

    if (*p == 0) {
        *path_ptr = p;
        return 0;
    }

    while (*p && *p != '/' && *p != '\\') {
        if (i + 1 < out_size) {
            out[i++] = *p;
        }

        p++;
    }

    out[i] = 0;
    *path_ptr = p;

    return i > 0;
}

static int find_entry_in_directory(
    u32 dir_cluster,
    const char* component,
    fat_found_entry_t* out
) {
    char lfn[FAT_MAX_LONG_NAME];
    int have_lfn = 0;
    u32 cluster = dir_cluster;

    str_clear(lfn, sizeof(lfn));

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            if (!ata_read_sector(first_lba + s, sector_buf)) {
                kernel_print("fat: directory read failed\n");
                return 0;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                fat_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return 0;
                }

                if (e->name[0] == 0xE5) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                if (entry_is_lfn(e)) {
                    collect_lfn_entry((const u8*)e, lfn);
                    have_lfn = 1;
                    continue;
                }

                if (entry_is_skippable(e)) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                char short_name[13];
                make_short_name_string(e, short_name);

                if (component_matches_entry(component, have_lfn ? lfn : 0, short_name)) {
                    fill_found_entry(out, e, have_lfn ? lfn : 0);
                    return 1;
                }

                str_clear(lfn, sizeof(lfn));
                have_lfn = 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return 0;
}

static int resolve_path(const char* path, fat_found_entry_t* out) {
    char component[FAT_MAX_COMPONENT];
    const char* p = path;
    u32 dir_cluster = root_cluster;
    fat_found_entry_t found;
    int got_component = 0;

    if (!path || !path[0] || str_eq_ci(path, "/")) {
        str_clear(out->long_name, sizeof(out->long_name));
        str_copy_safe(out->long_name, "/", sizeof(out->long_name));
        str_clear(out->short_name, sizeof(out->short_name));
        out->attr = FAT_ATTR_DIRECTORY;
        out->first_cluster = root_cluster;
        out->file_size = 0;
        out->is_directory = 1;
        return 1;
    }

    while (next_path_component(&p, component, sizeof(component))) {
        got_component = 1;

        if (!find_entry_in_directory(dir_cluster, component, &found)) {
            return 0;
        }

        while (*p == '/' || *p == '\\') {
            p++;
        }

        if (*p == 0) {
            *out = found;
            return 1;
        }

        if (!found.is_directory) {
            return 0;
        }

        dir_cluster = found.first_cluster;
    }

    return got_component ? 0 : 1;
}

static int resolve_parent_and_name(
    const char* path,
    u32* out_parent_cluster,
    char* out_name,
    u32 out_name_size
) {
    char component[FAT_MAX_COMPONENT];
    char last[FAT_MAX_COMPONENT];
    const char* p = path;
    u32 dir_cluster = root_cluster;
    fat_found_entry_t found;

    str_clear(last, sizeof(last));

    while (next_path_component(&p, component, sizeof(component))) {
        while (*p == '/' || *p == '\\') {
            p++;
        }

        if (*p == 0) {
            str_copy_safe(out_name, component, out_name_size);
            *out_parent_cluster = dir_cluster;
            return 1;
        }

        if (!find_entry_in_directory(dir_cluster, component, &found)) {
            return 0;
        }

        if (!found.is_directory) {
            return 0;
        }

        dir_cluster = found.first_cluster;
        str_copy_safe(last, component, sizeof(last));
    }

    return 0;
}

static void print_entry_name(const fat_found_entry_t* e) {
    if (e->long_name[0]) {
        kernel_print(e->long_name);
    } else {
        kernel_print(e->short_name);
    }
}

static void print_dec32(u32 value) {
    char buf[16];
    int i = 0;

    if (value == 0) {
        kernel_put_char('0');
        return;
    }

    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        kernel_put_char(buf[--i]);
    }
}

static void list_directory_cluster(u32 dir_cluster) {
    char lfn[FAT_MAX_LONG_NAME];
    int have_lfn = 0;
    u32 cluster = dir_cluster;

    str_clear(lfn, sizeof(lfn));

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            if (!ata_read_sector(first_lba + s, sector_buf)) {
                kernel_print("fat: directory read failed\n");
                return;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                fat_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return;
                }

                if (e->name[0] == 0xE5) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                if (entry_is_lfn(e)) {
                    collect_lfn_entry((const u8*)e, lfn);
                    have_lfn = 1;
                    continue;
                }

                /*
                 * If a real entry is skippable, discard any collected LFN.
                 * This prevents orphaned/leftover LFN entries from being
                 * attached to invalid entries.
                 */
                if (entry_is_skippable(e)) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                /*
                 * Extra safety: never print entries with empty generated names.
                 */
                char short_name[13];
                make_short_name_string(e, short_name);

                if (!short_name[0]) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                fat_found_entry_t info;
                fill_found_entry(&info, e, have_lfn ? lfn : 0);

                if (!info.long_name[0] && !info.short_name[0]) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                kernel_print("  ");
                print_entry_name(&info);

                if (info.is_directory) {
                    kernel_print("  <DIR>");
                } else {
                    kernel_print("  size=");
                    print_dec32(info.file_size);
                }

                kernel_print("\n");

                str_clear(lfn, sizeof(lfn));
                have_lfn = 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }
}

static int read_file_by_entry(
    const fat_found_entry_t* entry,
    u8* out_buf,
    u32 max_bytes,
    u32* out_size
) {
    u32 cluster = entry->first_cluster;
    u32 remaining = entry->file_size;
    u32 written = 0;

    if (entry->is_directory) {
        kernel_print("fat: path is a directory\n");
        return 0;
    }

    if (remaining == 0) {
        if (out_size) {
            *out_size = 0;
        }

        return 1;
    }

    if (!cluster_is_valid(cluster)) {
        kernel_print("fat: invalid first cluster\n");
        return 0;
    }

    while (remaining > 0) {
        if (!cluster_is_valid(cluster)) {
            kernel_print("fat: cluster chain ended too early\n");
            return 0;
        }

        u32 lba = cluster_to_lba(cluster);

        for (u32 sec = 0; sec < bpb.sectors_per_cluster && remaining > 0; sec++) {
            if (!ata_read_sector(lba + sec, sector_buf)) {
                kernel_print("fat: data sector read failed\n");
                return 0;
            }

            u32 to_copy = remaining < 512 ? remaining : 512;

            if (written + to_copy > max_bytes) {
                kernel_print("fat: file too large for buffer\n");
                return 0;
            }

            for (u32 k = 0; k < to_copy; k++) {
                out_buf[written + k] = sector_buf[k];
            }

            written += to_copy;
            remaining -= to_copy;
        }

        if (remaining == 0) {
            if (out_size) {
                *out_size = written;
            }

            return 1;
        }

        cluster = fat32_next_cluster(cluster);
    }

    if (out_size) {
        *out_size = written;
    }

    return 1;
}

static void mem_zero_local(void* ptr, u32 size) {
    u8* p = (u8*)ptr;

    for (u32 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void set_entry_first_cluster(fat_dir_entry_t* e, u32 cluster) {
    e->first_cluster_high = (u16)((cluster >> 16) & 0xFFFF);
    e->first_cluster_low = (u16)(cluster & 0xFFFF);
}

static u32 fat_total_clusters(void) {
    u32 data_start_rel = data_start_lba - FAT_FS_START_LBA;
    u32 data_sectors;

    if (total_sectors <= data_start_rel) {
        return 0;
    }

    data_sectors = total_sectors - data_start_rel;
    return (data_sectors / bpb.sectors_per_cluster) + 2;
}

static int fat32_set_cluster(u32 cluster, u32 value) {
    u32 fat_offset = cluster * 4;
    u32 fat_sector_offset = fat_offset / bpb.bytes_per_sector;
    u32 ent_offset = fat_offset % bpb.bytes_per_sector;

    value &= FAT32_CLUSTER_MASK;

    for (u32 fat_index = 0; fat_index < bpb.num_fats; fat_index++) {
        u32 sector = fat_start_lba + (fat_index * sectors_per_fat) + fat_sector_offset;

        if (!ata_read_sector(sector, sector_buf)) {
            kernel_print("fat: FAT sector read failed\n");
            return 0;
        }

        sector_buf[ent_offset]     = (u8)(value & 0xFF);
        sector_buf[ent_offset + 1] = (u8)((value >> 8) & 0xFF);
        sector_buf[ent_offset + 2] = (u8)((value >> 16) & 0xFF);
        sector_buf[ent_offset + 3] =
            (u8)((sector_buf[ent_offset + 3] & 0xF0) | ((value >> 24) & 0x0F));

        if (!ata_write_sector(sector, sector_buf)) {
            kernel_print("fat: FAT sector write failed\n");
            return 0;
        }
    }

    return 1;
}

static int fat32_zero_cluster(u32 cluster) {
    u32 first_lba = cluster_to_lba(cluster);

    mem_zero_local(sector_buf, 512);

    for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
        if (!ata_write_sector(first_lba + s, sector_buf)) {
            kernel_print("fat: zero cluster write failed\n");
            return 0;
        }
    }

    return 1;
}

static u32 fat32_alloc_cluster(void) {
    u32 max_cluster = fat_total_clusters();

    for (u32 cluster = 2; cluster < max_cluster; cluster++) {
        if (fat32_next_cluster(cluster) == 0) {
            if (!fat32_set_cluster(cluster, FAT32_EOC)) {
                return 0;
            }

            if (!fat32_zero_cluster(cluster)) {
                fat32_set_cluster(cluster, 0);
                return 0;
            }

            return cluster;
        }
    }

    return 0;
}

static int fat32_free_chain(u32 first_cluster) {
    u32 cluster = first_cluster;

    while (cluster_is_valid(cluster)) {
        u32 next = fat32_next_cluster(cluster);

        if (!fat32_set_cluster(cluster, 0)) {
            return 0;
        }

        cluster = next;
    }

    return 1;
}

static int make_short_name_83(const char* name, u8 out[11]) {
    int dot_seen = 0;
    int name_len = 0;
    int ext_len = 0;

    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    if (!name || !name[0]) {
        return 0;
    }

    if (str_eq_ci(name, ".") || str_eq_ci(name, "..")) {
        return 0;
    }

    for (u32 i = 0; name[i]; i++) {
        char c = name[i];

        if (c == '/' || c == '\\') {
            return 0;
        }

        if (c == '.') {
            if (dot_seen) {
                return 0;
            }

            dot_seen = 1;
            continue;
        }

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '~')) {
            return 0;
        }

        if (!dot_seen) {
            if (name_len >= 8) {
                return 0;
            }

            out[name_len++] = (u8)c;
        } else {
            if (ext_len >= 3) {
                return 0;
            }

            out[8 + ext_len++] = (u8)c;
        }
    }

    return name_len > 0;
}

static int find_free_entry_location(u32 dir_cluster, u32* out_sector, int* out_index) {
    u32 cluster = dir_cluster;
    u32 previous_cluster = 0;

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            u32 sector = first_lba + s;

            if (!ata_read_sector(sector, sector_buf)) {
                kernel_print("fat: directory read failed\n");
                return 0;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                    *out_sector = sector;
                    *out_index = i;
                    return 1;
                }
            }
        }

        previous_cluster = cluster;
        cluster = fat32_next_cluster(cluster);
    }

    /*
     * Directory is full. Extend it by one cluster.
     */
    u32 new_cluster = fat32_alloc_cluster();

    if (!new_cluster) {
        kernel_print("fat: no free cluster for directory growth\n");
        return 0;
    }

    if (!fat32_set_cluster(previous_cluster, new_cluster)) {
        fat32_set_cluster(new_cluster, 0);
        return 0;
    }

    if (!fat32_set_cluster(new_cluster, FAT32_EOC)) {
        return 0;
    }

    *out_sector = cluster_to_lba(new_cluster);
    *out_index = 0;
    return 1;
}

static int write_directory_entry_at(u32 sector, int index, const fat_dir_entry_t* entry) {
    if (!ata_read_sector(sector, sector_buf)) {
        kernel_print("fat: directory sector read failed\n");
        return 0;
    }

    fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;
    entries[index] = *entry;

    if (!ata_write_sector(sector, sector_buf)) {
        kernel_print("fat: directory sector write failed\n");
        return 0;
    }

    return 1;
}

static int find_entry_location_in_directory(
    u32 dir_cluster,
    const char* component,
    fat_found_entry_t* out,
    u32* out_sector,
    int* out_index
) {
    char lfn[FAT_MAX_LONG_NAME];
    int have_lfn = 0;
    u32 cluster = dir_cluster;

    str_clear(lfn, sizeof(lfn));

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            u32 sector = first_lba + s;

            if (!ata_read_sector(sector, sector_buf)) {
                kernel_print("fat: directory read failed\n");
                return 0;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                fat_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return 0;
                }

                if (e->name[0] == 0xE5) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                if (entry_is_lfn(e)) {
                    collect_lfn_entry((const u8*)e, lfn);
                    have_lfn = 1;
                    continue;
                }

                if (entry_is_skippable(e)) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                char short_name[13];
                make_short_name_string(e, short_name);

                if (component_matches_entry(component, have_lfn ? lfn : 0, short_name)) {
                    if (out) {
                        fill_found_entry(out, e, have_lfn ? lfn : 0);
                    }

                    if (out_sector) {
                        *out_sector = sector;
                    }

                    if (out_index) {
                        *out_index = i;
                    }

                    return 1;
                }

                str_clear(lfn, sizeof(lfn));
                have_lfn = 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return 0;
}

static int write_dot_entries(u32 dir_cluster, u32 parent_cluster) {
    u32 sector = cluster_to_lba(dir_cluster);

    mem_zero_local(sector_buf, 512);

    fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

    for (int i = 0; i < 11; i++) {
        entries[0].name[i] = ' ';
        entries[1].name[i] = ' ';
    }

    entries[0].name[0] = '.';
    entries[0].attr = FAT_ATTR_DIRECTORY;
    set_entry_first_cluster(&entries[0], dir_cluster);

    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    entries[1].attr = FAT_ATTR_DIRECTORY;

    if (parent_cluster == root_cluster) {
        set_entry_first_cluster(&entries[1], 0);
    } else {
        set_entry_first_cluster(&entries[1], parent_cluster);
    }

    if (!ata_write_sector(sector, sector_buf)) {
        return 0;
    }

    return 1;
}

void fat32_init(void) {
    fat_mounted = 0;

    if (!ata_read_sector(FAT_FS_START_LBA, sector_buf)) {
        kernel_print("fat: boot sector read failed\n");
        return;
    }

    fat32_bpb_t* on_disk = (fat32_bpb_t*)sector_buf;
    bpb = *on_disk;

    if (bpb.bytes_per_sector != 512) {
        kernel_print("fat: unsupported sector size\n");
        return;
    }

    if (bpb.sectors_per_cluster == 0) {
        kernel_print("fat: invalid sectors per cluster\n");
        return;
    }

    if (bpb.num_fats == 0) {
        kernel_print("fat: invalid FAT count\n");
        return;
    }

    if (bpb.root_entry_count != 0 || bpb.sectors_per_fat_16 != 0) {
        kernel_print("fat: expected FAT32 filesystem\n");
        return;
    }

    sectors_per_fat = bpb.sectors_per_fat_32;
    root_cluster = bpb.root_cluster;
    total_sectors = bpb.total_sectors_16 ? bpb.total_sectors_16 : bpb.total_sectors_32;

    if (sectors_per_fat == 0 || root_cluster < 2) {
        kernel_print("fat: invalid FAT32 BPB\n");
        return;
    }

    fat_start_lba = FAT_FS_START_LBA + (u32)bpb.reserved_sectors;
    data_start_lba =
        fat_start_lba +
        ((u32)bpb.num_fats * sectors_per_fat);

    fat_mounted = 1;

    kernel_print("fat: FAT32 initialized\n");
}

void fat32_list_root(void) {
    fat32_list_path("/");
}

int fat32_list_path(const char* path) {
    if (!fat_mounted) {
        kernel_print("fat: not mounted\n");
        return 0;
    }

    fat_found_entry_t entry;

    if (!resolve_path(path, &entry)) {
        kernel_print("fat: path not found\n");
        return 0;
    }

    if (!entry.is_directory) {
        print_entry_name(&entry);
        kernel_print("  size=");
        print_dec32(entry.file_size);
        kernel_print("\n");
        return 1;
    }

    kernel_print("Directory: ");
    if (!path || !path[0]) {
        kernel_print("/");
    } else {
        kernel_print(path);
    }
    kernel_print("\n");

    list_directory_cluster(entry.first_cluster);
    return 1;
}

int fat32_read_file_path(const char* path, u8* out_buf, u32 max_bytes, u32* out_size) {
    if (!fat_mounted) {
        kernel_print("fat: not mounted\n");
        return 0;
    }

    if (!path || !out_buf) {
        return 0;
    }

    fat_found_entry_t entry;

    if (!resolve_path(path, &entry)) {
        return 0;
    }

    return read_file_by_entry(&entry, out_buf, max_bytes, out_size);
}

/*
 * Legacy root short-name read.
 * name83 must be an 11-byte FAT short name, e.g. "KERNEL  BIN".
 */
int fat32_read_file(const char* name83, u8* out_buf, u32 max_bytes, u32* out_size) {
    if (!fat_mounted) {
        kernel_print("fat: not mounted\n");
        return 0;
    }

    if (!name83 || !out_buf) {
        return 0;
    }

    char short_name[13];
    int pos = 0;

    for (int i = 0; i < 8; i++) {
        if (name83[i] != ' ') {
            short_name[pos++] = name83[i];
        }
    }

    int has_ext = 0;
    for (int i = 8; i < 11; i++) {
        if (name83[i] != ' ') {
            has_ext = 1;
        }
    }

    if (has_ext) {
        short_name[pos++] = '.';

        for (int i = 8; i < 11; i++) {
            if (name83[i] != ' ') {
                short_name[pos++] = name83[i];
            }
        }
    }

    short_name[pos] = 0;

    return fat32_read_file_path(short_name, out_buf, max_bytes, out_size);
}

int fat32_load_file_to_memory(const char* name83, u8* dest, u32 max_bytes, u32* out_size) {
    return fat32_read_file(name83, dest, max_bytes, out_size);
}

int fat32_load_file_to_memory_path(const char* path, u8* dest, u32 max_bytes, u32* out_size) {
    return fat32_read_file_path(path, dest, max_bytes, out_size);
}

typedef struct {
    u32 sector;
    int index;
} fat_entry_location_t;

static u8 fat_lfn_checksum(const u8 short_name[11]) {
    u8 sum = 0;

    for (int i = 0; i < 11; i++) {
        sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    }

    return sum;
}

static int needs_lfn_name(const char* name) {
    int name_len = 0;
    int ext_len = 0;
    int dot_seen = 0;

    if (!name || !name[0]) {
        return 0;
    }

    for (u32 i = 0; name[i]; i++) {
        char c = name[i];

        if (c >= 'a' && c <= 'z') {
            return 1;
        }

        if (c == '.') {
            if (dot_seen) {
                return 1;
            }

            dot_seen = 1;
            continue;
        }

        if (!dot_seen) {
            name_len++;
        } else {
            ext_len++;
        }

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '~')) {
            return 1;
        }
    }

    if (name_len > 8 || ext_len > 3) {
        return 1;
    }

    return 0;
}

static void make_short_alias_from_long(const char* name, u8 out[11], int serial) {
    int out_i = 0;
    int ext_i = 0;
    const char* ext = 0;
    char serial_buf[8];
    int serial_len = 0;
    int serial_tmp = serial;

    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    for (u32 i = 0; name[i]; i++) {
        if (name[i] == '.') {
            ext = name + i + 1;
        }
    }

    if (serial_tmp <= 0) {
        serial_tmp = 1;
    }

    while (serial_tmp > 0 && serial_len < 7) {
        serial_buf[serial_len++] = (char)('0' + (serial_tmp % 10));
        serial_tmp /= 10;
    }

    int base_limit = 8 - 1 - serial_len;
    if (base_limit < 1) {
        base_limit = 1;
    }

    for (u32 i = 0; name[i] && name[i] != '.' && out_i < base_limit; i++) {
        char c = name[i];

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }

        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[out_i++] = (u8)c;
        } else if (c == '_' || c == '-') {
            out[out_i++] = (u8)c;
        }
    }

    if (out_i == 0) {
        out[out_i++] = 'X';
    }

    out[out_i++] = '~';

    while (serial_len > 0 && out_i < 8) {
        out[out_i++] = (u8)serial_buf[--serial_len];
    }

    if (ext) {
        for (u32 i = 0; ext[i] && ext_i < 3; i++) {
            char c = ext[i];

            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 32);
            }

            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                out[8 + ext_i++] = (u8)c;
            }
        }
    }
}

static int short_name_exists_in_directory(u32 dir_cluster, const u8 name83[11]) {
    u32 cluster = dir_cluster;

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            u32 sector = first_lba + s;

            if (!ata_read_sector(sector, sector_buf)) {
                return 1;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                fat_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return 0;
                }

                if (e->name[0] == 0xE5 || entry_is_lfn(e)) {
                    continue;
                }

                int same = 1;
                for (int j = 0; j < 11; j++) {
                    if (e->name[j] != name83[j]) {
                        same = 0;
                        break;
                    }
                }

                if (same) {
                    return 1;
                }
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return 0;
}

static int make_create_short_name(u32 parent_cluster, const char* name, u8 out[11], int* out_uses_lfn) {
    if (!needs_lfn_name(name)) {
        if (!make_short_name_83(name, out)) {
            return 0;
        }

        if (short_name_exists_in_directory(parent_cluster, out)) {
            return 0;
        }

        if (out_uses_lfn) {
            *out_uses_lfn = 0;
        }

        return 1;
    }

    for (int serial = 1; serial < 100; serial++) {
        make_short_alias_from_long(name, out, serial);

        if (!short_name_exists_in_directory(parent_cluster, out)) {
            if (out_uses_lfn) {
                *out_uses_lfn = 1;
            }

            return 1;
        }
    }

    return 0;
}

static u32 lfn_entry_count_for_name(const char* name) {
    u32 len = 0;

    while (name[len]) {
        len++;
    }

    return (len + 12) / 13;
}

static void write_lfn_chars(u8* raw, const char* name, u32 start_index) {
    u32 offsets[13] = {
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30
    };

    for (u32 i = 0; i < 13; i++) {
        u32 name_index = start_index + i;
        u16 ch;

        if (name[name_index]) {
            ch = (u16)((u8)name[name_index]);
        } else if (i == 0 || name[name_index - 1]) {
            ch = 0x0000;
        } else {
            ch = 0xFFFF;
        }

        raw[offsets[i]] = (u8)(ch & 0xFF);
        raw[offsets[i] + 1] = (u8)((ch >> 8) & 0xFF);
    }
}

static void make_lfn_entry(u8* raw, const char* long_name, u32 seq, u32 total, u8 checksum) {
    for (int i = 0; i < 32; i++) {
        raw[i] = 0;
    }

    raw[0] = (u8)seq;

    if (seq == total) {
        raw[0] |= 0x40;
    }

    raw[11] = FAT_ATTR_LFN;
    raw[12] = 0;
    raw[13] = checksum;
    raw[26] = 0;
    raw[27] = 0;

    write_lfn_chars(raw, long_name, (seq - 1) * 13);
}

static int dir_entry_location_by_offset(u32 dir_cluster, u32 entry_offset, fat_entry_location_t* out) {
    u32 entries_per_sector = 16;
    u32 entries_per_cluster = entries_per_sector * bpb.sectors_per_cluster;
    u32 cluster_skip = entry_offset / entries_per_cluster;
    u32 entry_in_cluster = entry_offset % entries_per_cluster;
    u32 cluster = dir_cluster;

    for (u32 i = 0; i < cluster_skip; i++) {
        cluster = fat32_next_cluster(cluster);

        if (!cluster_is_valid(cluster)) {
            return 0;
        }
    }

    out->sector = cluster_to_lba(cluster) + (entry_in_cluster / entries_per_sector);
    out->index = (int)(entry_in_cluster % entries_per_sector);
    return 1;
}

static int find_free_consecutive_entries(u32 dir_cluster, u32 needed, u32* out_start_offset) {
    u32 cluster = dir_cluster;
    u32 previous_cluster = 0;
    u32 offset = 0;
    u32 run_start = 0;
    u32 run_count = 0;

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            u32 sector = first_lba + s;

            if (!ata_read_sector(sector, sector_buf)) {
                kernel_print("fat: directory read failed\n");
                return 0;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                    if (run_count == 0) {
                        run_start = offset;
                    }

                    run_count++;

                    if (run_count >= needed) {
                        *out_start_offset = run_start;
                        return 1;
                    }
                } else {
                    run_count = 0;
                }

                offset++;
            }
        }

        previous_cluster = cluster;
        cluster = fat32_next_cluster(cluster);
    }

    /*
     * Extend directory by one cluster. This guarantees a fresh run
     * of entries in the new zeroed cluster.
     */
    u32 new_cluster = fat32_alloc_cluster();

    if (!new_cluster) {
        kernel_print("fat: no free cluster for directory growth\n");
        return 0;
    }

    if (!fat32_set_cluster(previous_cluster, new_cluster)) {
        fat32_set_cluster(new_cluster, 0);
        return 0;
    }

    if (!fat32_set_cluster(new_cluster, FAT32_EOC)) {
        return 0;
    }

    *out_start_offset = offset;
    return 1;
}

static int write_entry_by_offset(u32 dir_cluster, u32 entry_offset, const fat_dir_entry_t* entry) {
    fat_entry_location_t loc;

    if (!dir_entry_location_by_offset(dir_cluster, entry_offset, &loc)) {
        return 0;
    }

    return write_directory_entry_at(loc.sector, loc.index, entry);
}

static int mark_entry_deleted_by_offset(u32 dir_cluster, u32 entry_offset) {
    fat_entry_location_t loc;

    if (!dir_entry_location_by_offset(dir_cluster, entry_offset, &loc)) {
        return 0;
    }

    if (!ata_read_sector(loc.sector, sector_buf)) {
        return 0;
    }

    fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;
    entries[loc.index].name[0] = 0xE5;

    return ata_write_sector(loc.sector, sector_buf);
}

static int write_lfn_and_short_entries(
    u32 parent_cluster,
    u32 start_offset,
    const char* long_name,
    const u8 short_name[11],
    const fat_dir_entry_t* short_entry,
    int uses_lfn
) {
    if (!uses_lfn) {
        return write_entry_by_offset(parent_cluster, start_offset, short_entry);
    }

    u32 lfn_count = lfn_entry_count_for_name(long_name);
    u8 checksum = fat_lfn_checksum(short_name);

    /*
     * VFAT entries are stored physically in reverse order:
     * last sequence first, sequence 1 immediately before short entry.
     */
    for (u32 i = 0; i < lfn_count; i++) {
        u32 seq = lfn_count - i;
        fat_dir_entry_t lfn_entry;

        make_lfn_entry((u8*)&lfn_entry, long_name, seq, lfn_count, checksum);

        if (!write_entry_by_offset(parent_cluster, start_offset + i, &lfn_entry)) {
            return 0;
        }
    }

    return write_entry_by_offset(parent_cluster, start_offset + lfn_count, short_entry);
}

static int find_entry_location_with_lfn_range(
    u32 dir_cluster,
    const char* component,
    fat_found_entry_t* out,
    u32* out_lfn_start_offset,
    u32* out_short_offset
) {
    char lfn[FAT_MAX_LONG_NAME];
    int have_lfn = 0;
    u32 lfn_start_offset = 0;
    u32 cluster = dir_cluster;
    u32 offset = 0;

    str_clear(lfn, sizeof(lfn));

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            u32 sector = first_lba + s;

            if (!ata_read_sector(sector, sector_buf)) {
                kernel_print("fat: directory read failed\n");
                return 0;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                fat_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return 0;
                }

                if (e->name[0] == 0xE5) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    offset++;
                    continue;
                }

                if (entry_is_lfn(e)) {
                    if (!have_lfn) {
                        lfn_start_offset = offset;
                    }

                    collect_lfn_entry((const u8*)e, lfn);
                    have_lfn = 1;
                    offset++;
                    continue;
                }

                if (entry_is_skippable(e)) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    offset++;
                    continue;
                }

                char short_name[13];
                make_short_name_string(e, short_name);

                if (component_matches_entry(component, have_lfn ? lfn : 0, short_name)) {
                    if (out) {
                        fill_found_entry(out, e, have_lfn ? lfn : 0);
                    }

                    if (out_lfn_start_offset) {
                        *out_lfn_start_offset = have_lfn ? lfn_start_offset : offset;
                    }

                    if (out_short_offset) {
                        *out_short_offset = offset;
                    }

                    return 1;
                }

                str_clear(lfn, sizeof(lfn));
                have_lfn = 0;
                offset++;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return 0;
}

static int directory_is_empty(u32 dir_cluster) {
    u32 cluster = dir_cluster;

    while (cluster_is_valid(cluster)) {
        u32 first_lba = cluster_to_lba(cluster);

        for (u32 s = 0; s < bpb.sectors_per_cluster; s++) {
            if (!ata_read_sector(first_lba + s, sector_buf)) {
                return 0;
            }

            fat_dir_entry_t* entries = (fat_dir_entry_t*)sector_buf;

            for (int i = 0; i < 16; i++) {
                fat_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return 1;
                }

                if (e->name[0] == 0xE5 || entry_is_lfn(e)) {
                    continue;
                }

                if (e->attr & FAT_ATTR_VOLUME_ID) {
                    continue;
                }

                if (e->name[0] == '.') {
                    continue;
                }

                return 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return 1;
}

int fat32_mkdir_path(const char* path) {
    u32 parent_cluster;
    char name[FAT_MAX_COMPONENT];
    u8 name83[11];
    fat_found_entry_t existing;
    u32 start_offset;
    u32 needed_entries;
    u32 new_cluster;
    fat_dir_entry_t entry;
    int uses_lfn = 0;

    if (!fat_mounted) {
        kernel_print("fat: not mounted\n");
        return 0;
    }

    if (!resolve_parent_and_name(path, &parent_cluster, name, sizeof(name))) {
        kernel_print("fat: invalid parent path\n");
        return 0;
    }

    if (!make_create_short_name(parent_cluster, name, name83, &uses_lfn)) {
        kernel_print("fat: could not create short alias\n");
        return 0;
    }

    if (find_entry_in_directory(parent_cluster, name, &existing)) {
        kernel_print("fat: path already exists\n");
        return 0;
    }

    needed_entries = uses_lfn ? (lfn_entry_count_for_name(name) + 1) : 1;

    if (!find_free_consecutive_entries(parent_cluster, needed_entries, &start_offset)) {
        kernel_print("fat: no free directory entries\n");
        return 0;
    }

    new_cluster = fat32_alloc_cluster();
    if (!new_cluster) {
        kernel_print("fat: no free cluster\n");
        return 0;
    }

    if (!write_dot_entries(new_cluster, parent_cluster)) {
        fat32_set_cluster(new_cluster, 0);
        kernel_print("fat: could not initialize directory\n");
        return 0;
    }

    mem_zero_local(&entry, sizeof(entry));

    for (int i = 0; i < 11; i++) {
        entry.name[i] = name83[i];
    }

    entry.attr = FAT_ATTR_DIRECTORY;
    set_entry_first_cluster(&entry, new_cluster);
    entry.file_size = 0;

    if (!write_lfn_and_short_entries(
            parent_cluster,
            start_offset,
            name,
            name83,
            &entry,
            uses_lfn)) {
        fat32_free_chain(new_cluster);
        kernel_print("fat: could not write directory entry\n");
        return 0;
    }

    return 1;
}

int fat32_rm_path(const char* path) {
    u32 parent_cluster;
    char name[FAT_MAX_COMPONENT];
    fat_found_entry_t entry_info;
    u32 lfn_start_offset;
    u32 short_offset;

    if (!fat_mounted) {
        kernel_print("fat: not mounted\n");
        return 0;
    }

    if (!resolve_parent_and_name(path, &parent_cluster, name, sizeof(name))) {
        kernel_print("fat: invalid path\n");
        return 0;
    }

    if (!find_entry_location_with_lfn_range(
            parent_cluster,
            name,
            &entry_info,
            &lfn_start_offset,
            &short_offset)) {
        kernel_print("fat: file not found\n");
        return 0;
    }

    if (entry_info.is_directory) {
        kernel_print("fat: rm refuses directories; use rmdir\n");
        return 0;
    }

    if (entry_info.first_cluster >= 2) {
        if (!fat32_free_chain(entry_info.first_cluster)) {
            kernel_print("fat: failed to free cluster chain\n");
            return 0;
        }
    }

    for (u32 off = lfn_start_offset; off <= short_offset; off++) {
        if (!mark_entry_deleted_by_offset(parent_cluster, off)) {
            kernel_print("fat: failed to delete directory entry\n");
            return 0;
        }
    }

    return 1;
}

int fat32_rmdir_path(const char* path) {
    u32 parent_cluster;
    char name[FAT_MAX_COMPONENT];
    fat_found_entry_t entry_info;
    u32 lfn_start_offset;
    u32 short_offset;

    if (!fat_mounted) {
        kernel_print("fat: not mounted\n");
        return 0;
    }

    if (!path || !path[0] || str_eq_ci(path, "/")) {
        kernel_print("fat: refusing to remove root directory\n");
        return 0;
    }

    if (!resolve_parent_and_name(path, &parent_cluster, name, sizeof(name))) {
        kernel_print("fat: invalid path\n");
        return 0;
    }

    if (!find_entry_location_with_lfn_range(
            parent_cluster,
            name,
            &entry_info,
            &lfn_start_offset,
            &short_offset)) {
        kernel_print("fat: directory not found\n");
        return 0;
    }

    if (!entry_info.is_directory) {
        kernel_print("fat: path is not a directory\n");
        return 0;
    }

    if (!directory_is_empty(entry_info.first_cluster)) {
        kernel_print("fat: directory is not empty\n");
        return 0;
    }

    if (!fat32_free_chain(entry_info.first_cluster)) {
        kernel_print("fat: failed to free directory cluster chain\n");
        return 0;
    }

    for (u32 off = lfn_start_offset; off <= short_offset; off++) {
        if (!mark_entry_deleted_by_offset(parent_cluster, off)) {
            kernel_print("fat: failed to delete directory entry\n");
            return 0;
        }
    }

    return 1;
}