#include "fat32_64.h"

#include "../storage/block64.h"
#include "../k64_console.h"

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

#define FAT32_EOC           0x0FFFFFF8u
#define FAT32_BAD_CLUSTER   0x0FFFFFF7u
#define FAT32_CLUSTER_MASK  0x0FFFFFFFu

#define FAT32_MAX_LONG_NAME 128
#define FAT32_MAX_COMPONENT 128

typedef struct fat32_bpb {
    nq_u8  jump[3];
    nq_u8  oem[8];

    nq_u16 bytes_per_sector;
    nq_u8  sectors_per_cluster;
    nq_u16 reserved_sectors;
    nq_u8  num_fats;
    nq_u16 root_entry_count;
    nq_u16 total_sectors_16;
    nq_u8  media;
    nq_u16 sectors_per_fat_16;
    nq_u16 sectors_per_track;
    nq_u16 num_heads;
    nq_u32 hidden_sectors;
    nq_u32 total_sectors_32;

    nq_u32 sectors_per_fat_32;
    nq_u16 ext_flags;
    nq_u16 fs_version;
    nq_u32 root_cluster;
    nq_u16 fs_info;
    nq_u16 backup_boot_sector;
    nq_u8  reserved[12];
    nq_u8  drive_number;
    nq_u8  reserved1;
    nq_u8  boot_signature;
    nq_u32 volume_id;
    nq_u8  volume_label[11];
    nq_u8  fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

typedef struct fat32_dir_entry {
    nq_u8  name[11];
    nq_u8  attr;
    nq_u8  nt_reserved;
    nq_u8  create_time_tenths;
    nq_u16 create_time;
    nq_u16 create_date;
    nq_u16 access_date;
    nq_u16 first_cluster_high;
    nq_u16 write_time;
    nq_u16 write_date;
    nq_u16 first_cluster_low;
    nq_u32 file_size;
} __attribute__((packed)) fat32_dir_entry_t;

typedef struct fat32_lfn_entry {
    nq_u8  order;
    nq_u16 name1[5];
    nq_u8  attr;
    nq_u8  type;
    nq_u8  checksum;
    nq_u16 name2[6];
    nq_u16 zero;
    nq_u16 name3[2];
} __attribute__((packed)) fat32_lfn_entry_t;

typedef struct fat32_found_entry {
    char long_name[FAT32_MAX_LONG_NAME];
    char short_name[13];

    nq_u8 attr;
    nq_u32 first_cluster;
    nq_u32 file_size;
    int is_directory;
} fat32_found_entry_t;

static int g_fat_mounted = 0;

static nq_u64 g_base_lba = 0;
static nq_u32 g_total_sectors = 0;
static nq_u32 g_fat_start_lba = 0;
static nq_u32 g_data_start_lba = 0;
static nq_u32 g_sectors_per_fat = 0;
static nq_u32 g_root_cluster = 0;
static nq_u8  g_sectors_per_cluster = 0;
static nq_u8  g_num_fats = 0;

static nq_u8 g_sector_buf[BLOCK64_SECTOR_SIZE];
static nq_u8 g_fat_sector_buf[BLOCK64_SECTOR_SIZE];

static void mem_zero(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void str_clear(char* s, nq_u64 size) {
    if (!s) {
        return;
    }

    for (nq_u64 i = 0; i < size; i++) {
        s[i] = 0;
    }
}

static void str_copy_safe(char* dst, const char* src, nq_u64 dst_size) {
    nq_u64 i = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = 0;
        return;
    }

    while (src[i] && i < dst_size - 1) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static int ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + 32;
    }

    return c;
}

static int str_eq_ci(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }

    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return 0;
        }

        a++;
        b++;
    }

    return *a == 0 && *b == 0;
}

static int cluster_is_valid(nq_u32 cluster) {
    cluster &= FAT32_CLUSTER_MASK;

    return cluster >= 2 &&
           cluster < FAT32_EOC &&
           cluster != FAT32_BAD_CLUSTER;
}

static nq_u32 cluster_to_lba(nq_u32 cluster) {
    return g_data_start_lba +
           ((cluster - 2u) * (nq_u32)g_sectors_per_cluster);
}

static int read_sector_fs(nq_u32 fs_lba, void* out) {
    return block64_read(g_base_lba + fs_lba, out, 1);
}

static nq_u32 read_le32(const nq_u8* p) {
    return ((nq_u32)p[0]) |
           ((nq_u32)p[1] << 8) |
           ((nq_u32)p[2] << 16) |
           ((nq_u32)p[3] << 24);
}

static nq_u16 read_le16(const nq_u8* p) {
    return ((nq_u16)p[0]) |
           ((nq_u16)p[1] << 8);
}

static nq_u32 fat32_first_cluster(const fat32_dir_entry_t* e) {
    return ((nq_u32)e->first_cluster_high << 16) |
           ((nq_u32)e->first_cluster_low);
}

static int entry_is_lfn(const fat32_dir_entry_t* e) {
    return (e->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN;
}

static int entry_is_skippable(const fat32_dir_entry_t* e) {
    if (e->attr & FAT32_ATTR_VOLUME_ID) {
        return 1;
    }

    return 0;
}

static nq_u32 fat32_next_cluster(nq_u32 cluster) {
    nq_u32 fat_offset = (cluster & FAT32_CLUSTER_MASK) * 4u;
    nq_u32 fat_sector = g_fat_start_lba + (fat_offset / BLOCK64_SECTOR_SIZE);
    nq_u32 entry_offset = fat_offset % BLOCK64_SECTOR_SIZE;

    if (!read_sector_fs(fat_sector, g_fat_sector_buf)) {
        return FAT32_EOC;
    }

    return read_le32(g_fat_sector_buf + entry_offset) & FAT32_CLUSTER_MASK;
}

static void make_short_name_string(const fat32_dir_entry_t* e, char out[13]) {
    int pos = 0;
    int has_ext = 0;

    for (int i = 0; i < 8; i++) {
        if (e->name[i] != ' ') {
            out[pos++] = (char)e->name[i];
        }
    }

    for (int i = 8; i < 11; i++) {
        if (e->name[i] != ' ') {
            has_ext = 1;
            break;
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

static void lfn_put_char(char* lfn, nq_u32 index, nq_u16 ch) {
    if (index >= FAT32_MAX_LONG_NAME - 1) {
        return;
    }

    if (ch == 0x0000 || ch == 0xFFFF) {
        return;
    }

    if (ch < 128) {
        lfn[index] = (char)ch;
    } else {
        lfn[index] = '?';
    }
}

static void collect_lfn_entry(const fat32_lfn_entry_t* lfn_entry, char* lfn) {
    nq_u32 order = lfn_entry->order & 0x1Fu;

    if (order == 0) {
        return;
    }

    nq_u32 base = (order - 1u) * 13u;

    for (nq_u32 i = 0; i < 5; i++) {
        lfn_put_char(lfn, base + i, lfn_entry->name1[i]);
    }

    for (nq_u32 i = 0; i < 6; i++) {
        lfn_put_char(lfn, base + 5u + i, lfn_entry->name2[i]);
    }

    for (nq_u32 i = 0; i < 2; i++) {
        lfn_put_char(lfn, base + 11u + i, lfn_entry->name3[i]);
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
    fat32_found_entry_t* out,
    const fat32_dir_entry_t* e,
    const char* long_name
) {
    if (!out || !e) {
        return;
    }

    mem_zero(out, sizeof(*out));

    make_short_name_string(e, out->short_name);

    if (long_name && long_name[0]) {
        str_copy_safe(out->long_name, long_name, sizeof(out->long_name));
    } else {
        str_copy_safe(out->long_name, out->short_name, sizeof(out->long_name));
    }

    out->attr = e->attr;
    out->first_cluster = fat32_first_cluster(e);
    out->file_size = e->file_size;
    out->is_directory = (e->attr & FAT32_ATTR_DIRECTORY) != 0;
}

static int next_path_component(
    const char** inout,
    char* out,
    nq_u32 out_size
) {
    const char* p;
    nq_u32 len = 0;

    if (!inout || !*inout || !out || out_size == 0) {
        return 0;
    }

    p = *inout;

    while (*p == '/' || *p == '\\') {
        p++;
    }

    if (*p == 0) {
        *inout = p;
        return 0;
    }

    while (*p && *p != '/' && *p != '\\') {
        if (len + 1 < out_size) {
            out[len++] = *p;
        }

        p++;
    }

    out[len] = 0;
    *inout = p;

    return len > 0;
}

static int find_entry_in_directory(
    nq_u32 dir_cluster,
    const char* component,
    fat32_found_entry_t* out
) {
    char lfn[FAT32_MAX_LONG_NAME];
    int have_lfn = 0;
    nq_u32 cluster = dir_cluster;

    str_clear(lfn, sizeof(lfn));

    while (cluster_is_valid(cluster)) {
        nq_u32 first_lba = cluster_to_lba(cluster);

        for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
            if (!read_sector_fs(first_lba + s, g_sector_buf)) {
                k64_print("fat32: directory read failed\n");
                return 0;
            }

            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)g_sector_buf;

            for (int i = 0; i < 16; i++) {
                fat32_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return 0;
                }

                if (e->name[0] == 0xE5) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                if (entry_is_lfn(e)) {
                    collect_lfn_entry((const fat32_lfn_entry_t*)e, lfn);
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

static int resolve_path(const char* path, fat32_found_entry_t* out) {
    char component[FAT32_MAX_COMPONENT];
    const char* p = path;
    nq_u32 dir_cluster = g_root_cluster;
    fat32_found_entry_t found;
    int got_component = 0;

    if (!out) {
        return 0;
    }

    if (!path || !path[0] || str_eq_ci(path, "/")) {
        mem_zero(out, sizeof(*out));
        str_copy_safe(out->long_name, "/", sizeof(out->long_name));
        out->attr = FAT32_ATTR_DIRECTORY;
        out->first_cluster = g_root_cluster;
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

static void print_entry_name(const fat32_found_entry_t* entry) {
    if (!entry) {
        return;
    }

    if (entry->long_name[0]) {
        k64_print(entry->long_name);
    } else {
        k64_print(entry->short_name);
    }
}

static void list_directory_cluster(nq_u32 dir_cluster) {
    char lfn[FAT32_MAX_LONG_NAME];
    int have_lfn = 0;
    nq_u32 cluster = dir_cluster;

    str_clear(lfn, sizeof(lfn));

    while (cluster_is_valid(cluster)) {
        nq_u32 first_lba = cluster_to_lba(cluster);

        for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
            if (!read_sector_fs(first_lba + s, g_sector_buf)) {
                k64_print("fat32: directory read failed\n");
                return;
            }

            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)g_sector_buf;

            for (int i = 0; i < 16; i++) {
                fat32_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return;
                }

                if (e->name[0] == 0xE5) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                if (entry_is_lfn(e)) {
                    collect_lfn_entry((const fat32_lfn_entry_t*)e, lfn);
                    have_lfn = 1;
                    continue;
                }

                if (entry_is_skippable(e)) {
                    str_clear(lfn, sizeof(lfn));
                    have_lfn = 0;
                    continue;
                }

                fat32_found_entry_t entry;
                fill_found_entry(&entry, e, have_lfn ? lfn : 0);

                k64_print("  ");

                if (entry.is_directory) {
                    k64_print("[DIR]  ");
                } else {
                    k64_print("       ");
                }

                print_entry_name(&entry);

                if (!entry.is_directory) {
                    k64_print("  size=");
                    k64_print_dec64(entry.file_size);
                }

                k64_print("\n");

                str_clear(lfn, sizeof(lfn));
                have_lfn = 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }
}

static int read_file_by_entry(
    const fat32_found_entry_t* entry,
    nq_u8* out_buf,
    nq_u64 max_bytes,
    nq_u64* out_size
) {
    nq_u64 total_read = 0;
    nq_u32 cluster;

    if (out_size) {
        *out_size = 0;
    }

    if (!entry || !out_buf) {
        return 0;
    }

    if (entry->is_directory) {
        return 0;
    }

    if ((nq_u64)entry->file_size > max_bytes) {
        return 0;
    }

    cluster = entry->first_cluster;

    while (cluster_is_valid(cluster) && total_read < entry->file_size) {
        nq_u32 first_lba = cluster_to_lba(cluster);

        for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
            if (total_read >= entry->file_size) {
                break;
            }

            if (!read_sector_fs(first_lba + s, g_sector_buf)) {
                return 0;
            }

            nq_u64 remaining = (nq_u64)entry->file_size - total_read;
            nq_u64 take = BLOCK64_SECTOR_SIZE;

            if (take > remaining) {
                take = remaining;
            }

            for (nq_u64 i = 0; i < take; i++) {
                out_buf[total_read + i] = g_sector_buf[i];
            }

            total_read += take;
        }

        cluster = fat32_next_cluster(cluster);
    }

    if (out_size) {
        *out_size = total_read;
    }

    return total_read == entry->file_size;
}

int fat32_64_init(nq_u64 base_lba) {
    fat32_bpb_t* bpb;

    g_fat_mounted = 0;
    g_base_lba = base_lba;

    if (!block64_available()) {
        k64_print("fat32: no block device\n");
        return 0;
    }

    if (!block64_read(g_base_lba, g_sector_buf, 1)) {
        k64_print("fat32: boot sector read failed\n");
        return 0;
    }

    bpb = (fat32_bpb_t*)g_sector_buf;

    if (bpb->bytes_per_sector != BLOCK64_SECTOR_SIZE) {
        k64_print("fat32: unsupported sector size\n");
        return 0;
    }

    if (bpb->sectors_per_cluster == 0) {
        k64_print("fat32: invalid sectors per cluster\n");
        return 0;
    }

    if (bpb->num_fats == 0) {
        k64_print("fat32: invalid FAT count\n");
        return 0;
    }

    if (bpb->root_entry_count != 0 || bpb->sectors_per_fat_16 != 0) {
        k64_print("fat32: expected FAT32 filesystem\n");
        return 0;
    }

    g_sectors_per_fat = bpb->sectors_per_fat_32;
    g_root_cluster = bpb->root_cluster;
    g_total_sectors = bpb->total_sectors_16 ?
        bpb->total_sectors_16 :
        bpb->total_sectors_32;
    g_sectors_per_cluster = bpb->sectors_per_cluster;
    g_num_fats = bpb->num_fats;

    if (g_sectors_per_fat == 0 || g_root_cluster < 2) {
        k64_print("fat32: invalid FAT32 BPB\n");
        return 0;
    }

    g_fat_start_lba = (nq_u32)bpb->reserved_sectors;
    g_data_start_lba =
        g_fat_start_lba +
        ((nq_u32)bpb->num_fats * g_sectors_per_fat);

    g_fat_mounted = 1;

    k64_print("fat32: mounted read-only\n");
    return 1;
}

int fat32_64_mounted(void) {
    return g_fat_mounted;
}

void fat32_64_print_summary(void) {
    k64_print("FAT32: ");

    if (!g_fat_mounted) {
        k64_print("not mounted\n");
        return;
    }

    k64_print("mounted read-only\n");

    k64_print("Base LBA: ");
    k64_print_dec64(g_base_lba);
    k64_print("\n");

    k64_print("Total sectors: ");
    k64_print_dec64(g_total_sectors);
    k64_print("\n");

    k64_print("Sectors/cluster: ");
    k64_print_dec64(g_sectors_per_cluster);
    k64_print("\n");

    k64_print("FAT start LBA: ");
    k64_print_dec64(g_fat_start_lba);
    k64_print("\n");

    k64_print("Data start LBA: ");
    k64_print_dec64(g_data_start_lba);
    k64_print("\n");

    k64_print("Root cluster: ");
    k64_print_dec64(g_root_cluster);
    k64_print("\n");
}

int fat32_64_list_path(const char* path) {
    fat32_found_entry_t entry;

    if (!g_fat_mounted) {
        k64_print("fat32: not mounted\n");
        return 0;
    }

    if (!resolve_path(path, &entry)) {
        k64_print("fat32: path not found\n");
        return 0;
    }

    if (!entry.is_directory) {
        print_entry_name(&entry);
        k64_print("  size=");
        k64_print_dec64(entry.file_size);
        k64_print("\n");
        return 1;
    }

    k64_print("Directory: ");

    if (!path || !path[0]) {
        k64_print("/");
    } else {
        k64_print(path);
    }

    k64_print("\n");

    list_directory_cluster(entry.first_cluster);
    return 1;
}

int fat32_64_read_file_path(
    const char* path,
    nq_u8* out_buf,
    nq_u64 max_bytes,
    nq_u64* out_size
) {
    fat32_found_entry_t entry;

    if (!g_fat_mounted) {
        k64_print("fat32: not mounted\n");
        return 0;
    }

    if (!path || !out_buf) {
        return 0;
    }

    if (!resolve_path(path, &entry)) {
        return 0;
    }

    return read_file_by_entry(&entry, out_buf, max_bytes, out_size);
}