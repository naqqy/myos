#include "fat32_64.h"

#include "../storage/block64.h"
#include "../k64_console.h"
#include "../debug/bootlog64.h"

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
static nq_u32 g_total_clusters = 0;

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

    if (cluster < 2) {
        return 0;
    }

    if (cluster >= FAT32_EOC) {
        return 0;
    }

    if (cluster == FAT32_BAD_CLUSTER) {
        return 0;
    }

    if (g_total_clusters != 0 &&
        cluster >= g_total_clusters + 2u) {
        return 0;
    }

    return 1;
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
    cluster &= FAT32_CLUSTER_MASK;

    if (cluster < 2) {
        return FAT32_EOC;
    }

    if (g_total_clusters != 0 &&
        cluster >= g_total_clusters + 2u) {
        return FAT32_EOC;
    }

    nq_u32 fat_offset = cluster * 4u;
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

/*
 * Calls cb once for every real entry in a directory (skips deleted, volume
 * label and long-name helper entries).
 */
static void enum_directory_cluster(
    nq_u32 dir_cluster,
    fat32_64_enum_cb_t cb,
    void* ctx
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

                /*
                 * The callback may read other sectors (and so reuse
                 * g_sector_buf), which would invalidate `entries`. Copy the
                 * sector back in afterwards.
                 */
                cb(
                    entry.long_name[0] ? entry.long_name : entry.short_name,
                    entry.is_directory ? 0 : (nq_u64)entry.file_size,
                    entry.is_directory,
                    ctx
                );

                if (!read_sector_fs(first_lba + s, g_sector_buf)) {
                    return;
                }

                str_clear(lfn, sizeof(lfn));
                have_lfn = 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }
}

static void list_print_entry(
    const char* name,
    nq_u64 size,
    int is_dir,
    void* ctx
) {
    (void)ctx;

    k64_print("  ");
    k64_print(is_dir ? "[DIR]  " : "       ");
    k64_print(name);

    if (!is_dir) {
        k64_print("  size=");
        k64_print_dec64(size);
    }

    k64_print("\n");
}

static void list_directory_cluster(nq_u32 dir_cluster) {
    enum_directory_cluster(dir_cluster, list_print_entry, 0);
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

typedef struct fat32_entry_location {
    nq_u32 dir_cluster;
    nq_u32 sector_lba;
    nq_u32 entry_index;
    fat32_found_entry_t found;
} fat32_entry_location_t;

static int write_sector_fs(nq_u32 fs_lba, const void* in) {
    return block64_write(g_base_lba + fs_lba, in, 1);
}

static void write_le16(nq_u8* p, nq_u16 v) {
    p[0] = (nq_u8)(v & 0xFFu);
    p[1] = (nq_u8)((v >> 8) & 0xFFu);
}

static void write_le32(nq_u8* p, nq_u32 v) {
    p[0] = (nq_u8)(v & 0xFFu);
    p[1] = (nq_u8)((v >> 8) & 0xFFu);
    p[2] = (nq_u8)((v >> 16) & 0xFFu);
    p[3] = (nq_u8)((v >> 24) & 0xFFu);
}

static void fat32_set_first_cluster(fat32_dir_entry_t* e, nq_u32 cluster) {
    e->first_cluster_low = (nq_u16)(cluster & 0xFFFFu);
    e->first_cluster_high = (nq_u16)((cluster >> 16) & 0xFFFFu);
}

static int fat32_write_fat_entry(nq_u32 cluster, nq_u32 value) {
    cluster &= FAT32_CLUSTER_MASK;
    value &= FAT32_CLUSTER_MASK;

    if (cluster < 2 || cluster >= g_total_clusters + 2u) {
        return 0;
    }

    nq_u32 fat_offset = cluster * 4u;
    nq_u32 fat_sector_offset = fat_offset / BLOCK64_SECTOR_SIZE;
    nq_u32 entry_offset = fat_offset % BLOCK64_SECTOR_SIZE;

    for (nq_u32 fat = 0; fat < g_num_fats; fat++) {
        nq_u32 sector =
            g_fat_start_lba +
            (fat * g_sectors_per_fat) +
            fat_sector_offset;

        if (!read_sector_fs(sector, g_fat_sector_buf)) {
            return 0;
        }

        write_le32(g_fat_sector_buf + entry_offset, value);

        if (!write_sector_fs(sector, g_fat_sector_buf)) {
            return 0;
        }
    }

    return 1;
}

static nq_u32 fat32_find_free_cluster(void) {
    for (nq_u32 cluster = 2; cluster < g_total_clusters + 2u; cluster++) {
        if (fat32_next_cluster(cluster) == 0) {
            return cluster;
        }
    }

    return 0;
}

static int fat32_zero_cluster(nq_u32 cluster) {
    if (!cluster_is_valid(cluster)) {
        return 0;
    }

    for (nq_u32 i = 0; i < BLOCK64_SECTOR_SIZE; i++) {
        g_sector_buf[i] = 0;
    }

    nq_u32 first_lba = cluster_to_lba(cluster);

    for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
        if (!write_sector_fs(first_lba + s, g_sector_buf)) {
            return 0;
        }
    }

    return 1;
}

static void fat32_free_cluster_chain(nq_u32 first_cluster) {
    nq_u32 cluster = first_cluster & FAT32_CLUSTER_MASK;

    while (cluster_is_valid(cluster)) {
        nq_u32 next = fat32_next_cluster(cluster);

        fat32_write_fat_entry(cluster, 0);

        if (!cluster_is_valid(next)) {
            break;
        }

        cluster = next;
    }
}

static int fat32_alloc_cluster_chain(nq_u32 count, nq_u32* out_first) {
    nq_u32 first = 0;
    nq_u32 prev = 0;

    if (out_first) {
        *out_first = 0;
    }

    if (count == 0) {
        return 1;
    }

    for (nq_u32 i = 0; i < count; i++) {
        nq_u32 cluster = fat32_find_free_cluster();

        if (!cluster) {
            if (first) {
                fat32_free_cluster_chain(first);
            }

            return 0;
        }

        if (!fat32_write_fat_entry(cluster, FAT32_EOC)) {
            if (first) {
                fat32_free_cluster_chain(first);
            }

            return 0;
        }

        if (!fat32_zero_cluster(cluster)) {
            fat32_free_cluster_chain(cluster);

            if (first) {
                fat32_free_cluster_chain(first);
            }

            return 0;
        }

        if (!first) {
            first = cluster;
        }

        if (prev) {
            if (!fat32_write_fat_entry(prev, cluster)) {
                fat32_free_cluster_chain(first);
                return 0;
            }
        }

        prev = cluster;
    }

    if (out_first) {
        *out_first = first;
    }

    return 1;
}

static int fat32_is_valid_short_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }

    if (c >= 'a' && c <= 'z') {
        return 1;
    }

    if (c >= '0' && c <= '9') {
        return 1;
    }

    switch (c) {
        case '$':
        case '%':
        case '\'':
        case '-':
        case '_':
        case '@':
        case '~':
        case '`':
        case '!':
        case '(':
        case ')':
        case '{':
        case '}':
        case '^':
        case '#':
        case '&':
            return 1;
        default:
            return 0;
    }
}

static char fat32_upper_char(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 32);
    }

    return c;
}

static int fat32_make_short_name_from_component(const char* name, nq_u8 out[11]) {
    int name_len = 0;
    int ext_len = 0;
    int dot_seen = 0;

    if (!name || !name[0]) {
        return 0;
    }

    if (str_eq_ci(name, ".") || str_eq_ci(name, "..")) {
        return 0;
    }

    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    for (int i = 0; name[i]; i++) {
        char c = name[i];

        if (c == '.') {
            if (dot_seen) {
                return 0;
            }

            dot_seen = 1;
            continue;
        }

        if (!fat32_is_valid_short_char(c)) {
            return 0;
        }

        if (!dot_seen) {
            if (name_len >= 8) {
                return 0;
            }

            out[name_len++] = (nq_u8)fat32_upper_char(c);
        } else {
            if (ext_len >= 3) {
                return 0;
            }

            out[8 + ext_len++] = (nq_u8)fat32_upper_char(c);
        }
    }

    return name_len > 0;
}

static int fat32_split_parent_path(
    const char* path,
    char* out_parent,
    nq_u32 parent_size,
    char* out_name,
    nq_u32 name_size
) {
    nq_u32 len = 0;
    nq_u32 end;
    nq_u32 last_sep = 0xFFFFFFFFu;

    if (!path || !path[0] || !out_parent || !out_name ||
        parent_size == 0 || name_size == 0) {
        return 0;
    }

    while (path[len]) {
        len++;
    }

    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        len--;
    }

    if (len == 0 || (len == 1 && (path[0] == '/' || path[0] == '\\'))) {
        return 0;
    }

    end = len;

    for (nq_u32 i = 0; i < end; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            last_sep = i;
        }
    }

    if (last_sep == 0xFFFFFFFFu) {
        str_copy_safe(out_parent, "/", parent_size);

        nq_u32 n = 0;
        while (n < end && n + 1 < name_size) {
            out_name[n] = path[n];
            n++;
        }

        out_name[n] = 0;
        return n > 0;
    }

    if (last_sep == 0) {
        str_copy_safe(out_parent, "/", parent_size);
    } else {
        if (last_sep + 1 > parent_size) {
            return 0;
        }

        for (nq_u32 i = 0; i < last_sep; i++) {
            out_parent[i] = path[i];
        }

        out_parent[last_sep] = 0;
    }

    nq_u32 name_len = end - last_sep - 1;

    if (name_len == 0 || name_len + 1 > name_size) {
        return 0;
    }

    for (nq_u32 i = 0; i < name_len; i++) {
        out_name[i] = path[last_sep + 1 + i];
    }

    out_name[name_len] = 0;

    return 1;
}

static int fat32_find_entry_location(
    nq_u32 dir_cluster,
    const char* component,
    fat32_entry_location_t* out
) {
    char lfn[FAT32_MAX_LONG_NAME];
    int have_lfn = 0;
    nq_u32 cluster = dir_cluster;

    if (!out) {
        return 0;
    }

    str_clear(lfn, sizeof(lfn));

    while (cluster_is_valid(cluster)) {
        nq_u32 first_lba = cluster_to_lba(cluster);

        for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
            nq_u32 sector_lba = first_lba + s;

            if (!read_sector_fs(sector_lba, g_sector_buf)) {
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
                    out->dir_cluster = dir_cluster;
                    out->sector_lba = sector_lba;
                    out->entry_index = (nq_u32)i;
                    fill_found_entry(&out->found, e, have_lfn ? lfn : 0);
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

static int fat32_find_free_dir_entry(
    nq_u32 dir_cluster,
    nq_u32* out_sector_lba,
    nq_u32* out_entry_index
) {
    nq_u32 cluster = dir_cluster;
    nq_u32 prev_cluster = 0;

    while (cluster_is_valid(cluster)) {
        nq_u32 first_lba = cluster_to_lba(cluster);

        for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
            nq_u32 sector_lba = first_lba + s;

            if (!read_sector_fs(sector_lba, g_sector_buf)) {
                return 0;
            }

            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)g_sector_buf;

            for (nq_u32 i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0x00 ||
                    entries[i].name[0] == 0xE5) {
                    *out_sector_lba = sector_lba;
                    *out_entry_index = i;
                    return 1;
                }
            }
        }

        prev_cluster = cluster;

        nq_u32 next = fat32_next_cluster(cluster);

        if (!cluster_is_valid(next)) {
            break;
        }

        cluster = next;
    }

    nq_u32 new_cluster = 0;

    if (!fat32_alloc_cluster_chain(1, &new_cluster)) {
        return 0;
    }

    if (!fat32_write_fat_entry(prev_cluster, new_cluster)) {
        fat32_free_cluster_chain(new_cluster);
        return 0;
    }

    *out_sector_lba = cluster_to_lba(new_cluster);
    *out_entry_index = 0;

    return 1;
}

static int fat32_write_dir_entry_at(
    nq_u32 sector_lba,
    nq_u32 entry_index,
    const fat32_dir_entry_t* entry
) {
    if (entry_index >= 16 || !entry) {
        return 0;
    }

    if (!read_sector_fs(sector_lba, g_sector_buf)) {
        return 0;
    }

    fat32_dir_entry_t* entries = (fat32_dir_entry_t*)g_sector_buf;
    entries[entry_index] = *entry;

    return write_sector_fs(sector_lba, g_sector_buf);
}

static void fat32_build_dir_entry(
    fat32_dir_entry_t* e,
    const nq_u8 short_name[11],
    nq_u8 attr,
    nq_u32 first_cluster,
    nq_u32 file_size
) {
    mem_zero(e, sizeof(*e));

    for (int i = 0; i < 11; i++) {
        e->name[i] = short_name[i];
    }

    e->attr = attr;
    fat32_set_first_cluster(e, first_cluster);
    e->file_size = file_size;
}

static int fat32_write_file_data_to_chain(
    nq_u32 first_cluster,
    const nq_u8* data,
    nq_u64 size
) {
    nq_u64 written = 0;
    nq_u32 cluster = first_cluster;

    while (cluster_is_valid(cluster) && written < size) {
        nq_u32 first_lba = cluster_to_lba(cluster);

        for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
            for (nq_u32 i = 0; i < BLOCK64_SECTOR_SIZE; i++) {
                g_sector_buf[i] = 0;
            }

            nq_u64 remaining = size - written;
            nq_u64 take = BLOCK64_SECTOR_SIZE;

            if (take > remaining) {
                take = remaining;
            }

            for (nq_u64 i = 0; i < take; i++) {
                g_sector_buf[i] = data[written + i];
            }

            if (!write_sector_fs(first_lba + s, g_sector_buf)) {
                return 0;
            }

            written += take;

            if (written >= size) {
                break;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return written == size;
}

static int fat32_directory_is_empty(nq_u32 dir_cluster) {
    nq_u32 cluster = dir_cluster;

    while (cluster_is_valid(cluster)) {
        nq_u32 first_lba = cluster_to_lba(cluster);

        for (nq_u32 s = 0; s < g_sectors_per_cluster; s++) {
            if (!read_sector_fs(first_lba + s, g_sector_buf)) {
                return 0;
            }

            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)g_sector_buf;

            for (int i = 0; i < 16; i++) {
                fat32_dir_entry_t* e = &entries[i];

                if (e->name[0] == 0x00) {
                    return 1;
                }

                if (e->name[0] == 0xE5) {
                    continue;
                }

                if (entry_is_lfn(e)) {
                    continue;
                }

                if (entry_is_skippable(e)) {
                    continue;
                }

                if (e->name[0] == '.' &&
                    (e->name[1] == ' ' || e->name[1] == '.')) {
                    continue;
                }

                return 0;
            }
        }

        cluster = fat32_next_cluster(cluster);
    }

    return 1;
}

static int fat32_mark_entry_deleted(nq_u32 sector_lba, nq_u32 entry_index) {
    if (entry_index >= 16) {
        return 0;
    }

    if (!read_sector_fs(sector_lba, g_sector_buf)) {
        return 0;
    }

    fat32_dir_entry_t* entries = (fat32_dir_entry_t*)g_sector_buf;
    entries[entry_index].name[0] = 0xE5;

    return write_sector_fs(sector_lba, g_sector_buf);
}

static int fat32_init_directory_cluster(nq_u32 cluster, nq_u32 parent_cluster) {
    if (!fat32_zero_cluster(cluster)) {
        return 0;
    }

    nq_u32 first_lba = cluster_to_lba(cluster);

    if (!read_sector_fs(first_lba, g_sector_buf)) {
        return 0;
    }

    fat32_dir_entry_t* entries = (fat32_dir_entry_t*)g_sector_buf;

    nq_u8 dot_name[11] = {
        '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };

    nq_u8 dotdot_name[11] = {
        '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };

    fat32_build_dir_entry(
        &entries[0],
        dot_name,
        FAT32_ATTR_DIRECTORY,
        cluster,
        0
    );

    fat32_build_dir_entry(
        &entries[1],
        dotdot_name,
        FAT32_ATTR_DIRECTORY,
        parent_cluster,
        0
    );

    return write_sector_fs(first_lba, g_sector_buf);
}

static void fat32_free_cluster_chain_checked(nq_u32 first_cluster) {
    nq_u32 cluster = first_cluster & FAT32_CLUSTER_MASK;
    nq_u32 max_steps = g_total_clusters + 2u;
    nq_u32 steps = 0;

    while (cluster_is_valid(cluster) && steps < max_steps) {
        nq_u32 next = fat32_next_cluster(cluster);

        fat32_write_fat_entry(cluster, 0);

        if (!cluster_is_valid(next)) {
            break;
        }

        cluster = next;
        steps++;
    }

    if (steps >= max_steps) {
        k64_print("fat32: warning, stopped suspicious free chain\n");
    }
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

    if (g_total_sectors <= g_data_start_lba) {
        k64_print("fat32: invalid data start\n");
        return 0;
    }

    g_total_clusters =
        (g_total_sectors - g_data_start_lba) /
        (nq_u32)g_sectors_per_cluster;

    g_fat_mounted = 1;

    bootlog64_print("fat32: mounted read-only\n");
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

    k64_print("Total clusters: ");
    k64_print_dec64(g_total_clusters);
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

int fat32_64_enum_path(const char* path, fat32_64_enum_cb_t cb, void* ctx) {
    fat32_found_entry_t entry;

    if (!g_fat_mounted || !cb) {
        return 0;
    }

    if (!resolve_path(path, &entry) || !entry.is_directory) {
        return 0;
    }

    enum_directory_cluster(entry.first_cluster, cb, ctx);

    return 1;
}

int fat32_64_stat_path(const char* path, nq_u64* out_size, int* out_is_dir) {
    fat32_found_entry_t entry;

    if (!g_fat_mounted || !path) {
        return 0;
    }

    if (!resolve_path(path, &entry)) {
        return 0;
    }

    if (out_size) {
        *out_size = entry.is_directory ? 0 : (nq_u64)entry.file_size;
    }

    if (out_is_dir) {
        *out_is_dir = entry.is_directory ? 1 : 0;
    }

    return 1;
}

int fat32_64_write_file_path(
    const char* path,
    const nq_u8* data,
    nq_u64 size
) {
    char parent_path[FAT32_MAX_LONG_NAME];
    char name[FAT32_MAX_COMPONENT];
    nq_u8 short_name[11];

    fat32_found_entry_t parent;
    fat32_entry_location_t existing;
    nq_u32 sector_lba = 0;
    nq_u32 entry_index = 0;

    nq_u32 new_first_cluster = 0;
    nq_u32 old_first_cluster = 0;
    nq_u32 cluster_count = 0;

    if (!g_fat_mounted || !path) {
        return 0;
    }

    if (size > 0 && !data) {
        return 0;
    }

    if (size > 0xFFFFFFFFull) {
        return 0;
    }

    if (!fat32_split_parent_path(
            path,
            parent_path,
            sizeof(parent_path),
            name,
            sizeof(name))) {
        return 0;
    }

    if (!fat32_make_short_name_from_component(name, short_name)) {
        k64_print("fat32: write supports new 8.3 names only for now\n");
        return 0;
    }

    if (!resolve_path(parent_path, &parent) || !parent.is_directory) {
        return 0;
    }

    nq_u32 cluster_size =
        (nq_u32)g_sectors_per_cluster * BLOCK64_SECTOR_SIZE;

    if (size > 0) {
        cluster_count = (nq_u32)((size + cluster_size - 1u) / cluster_size);

        if (!fat32_alloc_cluster_chain(cluster_count, &new_first_cluster)) {
            return 0;
        }

        if (!fat32_write_file_data_to_chain(new_first_cluster, data, size)) {
            fat32_free_cluster_chain(new_first_cluster);
            return 0;
        }
    }

    if (fat32_find_entry_location(parent.first_cluster, name, &existing)) {
        if (existing.found.is_directory) {
            if (new_first_cluster) {
                fat32_free_cluster_chain(new_first_cluster);
            }

            return 0;
        }

        sector_lba = existing.sector_lba;
        entry_index = existing.entry_index;
        old_first_cluster = existing.found.first_cluster;
    } else {
        if (!fat32_find_free_dir_entry(
                parent.first_cluster,
                &sector_lba,
                &entry_index)) {
            if (new_first_cluster) {
                fat32_free_cluster_chain(new_first_cluster);
            }

            return 0;
        }
    }

    fat32_dir_entry_t entry;

    fat32_build_dir_entry(
        &entry,
        short_name,
        FAT32_ATTR_ARCHIVE,
        new_first_cluster,
        (nq_u32)size
    );

    if (!fat32_write_dir_entry_at(sector_lba, entry_index, &entry)) {
        if (new_first_cluster) {
            fat32_free_cluster_chain(new_first_cluster);
        }

        return 0;
    }

    /*
    * TEMP POC SAFETY:
    * Do not free old overwrite chains yet.
    * This leaks clusters during repeated append/overwrite, but avoids possible
    * FAT chain corruption while write support is still new.
    */
    /*
    if (old_first_cluster) {
        fat32_free_cluster_chain(old_first_cluster);
    }
    */

    return 1;
}

int fat32_64_mkdir_path(const char* path) {
    char parent_path[FAT32_MAX_LONG_NAME];
    char name[FAT32_MAX_COMPONENT];
    nq_u8 short_name[11];

    fat32_found_entry_t parent;
    fat32_found_entry_t exists;
    nq_u32 new_cluster = 0;
    nq_u32 sector_lba = 0;
    nq_u32 entry_index = 0;

    if (!g_fat_mounted || !path) {
        return 0;
    }

    if (!fat32_split_parent_path(
            path,
            parent_path,
            sizeof(parent_path),
            name,
            sizeof(name))) {
        return 0;
    }

    if (!fat32_make_short_name_from_component(name, short_name)) {
        k64_print("fat32: mkdir supports 8.3 names only for now\n");
        return 0;
    }

    if (!resolve_path(parent_path, &parent) || !parent.is_directory) {
        return 0;
    }

    if (find_entry_in_directory(parent.first_cluster, name, &exists)) {
        return 0;
    }

    if (!fat32_alloc_cluster_chain(1, &new_cluster)) {
        return 0;
    }

    if (!fat32_init_directory_cluster(new_cluster, parent.first_cluster)) {
        fat32_free_cluster_chain(new_cluster);
        return 0;
    }

    if (!fat32_find_free_dir_entry(
            parent.first_cluster,
            &sector_lba,
            &entry_index)) {
        fat32_free_cluster_chain(new_cluster);
        return 0;
    }

    fat32_dir_entry_t entry;

    fat32_build_dir_entry(
        &entry,
        short_name,
        FAT32_ATTR_DIRECTORY,
        new_cluster,
        0
    );

    if (!fat32_write_dir_entry_at(sector_lba, entry_index, &entry)) {
        fat32_free_cluster_chain(new_cluster);
        return 0;
    }

    return 1;
}

int fat32_64_rm_path(const char* path) {
    char parent_path[FAT32_MAX_LONG_NAME];
    char name[FAT32_MAX_COMPONENT];
    fat32_found_entry_t parent;
    fat32_entry_location_t loc;

    if (!g_fat_mounted || !path) {
        return 0;
    }

    if (!fat32_split_parent_path(
            path,
            parent_path,
            sizeof(parent_path),
            name,
            sizeof(name))) {
        return 0;
    }

    if (!resolve_path(parent_path, &parent) || !parent.is_directory) {
        return 0;
    }

    if (!fat32_find_entry_location(parent.first_cluster, name, &loc)) {
        return 0;
    }

    if (loc.found.is_directory) {
        return 0;
    }

    if (!fat32_mark_entry_deleted(loc.sector_lba, loc.entry_index)) {
        return 0;
    }

    if (loc.found.first_cluster) {
        fat32_free_cluster_chain(loc.found.first_cluster);
    }

    return 1;
}

int fat32_64_rmdir_path(const char* path) {
    char parent_path[FAT32_MAX_LONG_NAME];
    char name[FAT32_MAX_COMPONENT];
    fat32_found_entry_t parent;
    fat32_entry_location_t loc;

    if (!g_fat_mounted || !path) {
        return 0;
    }

    if (str_eq_ci(path, "/")) {
        return 0;
    }

    if (!fat32_split_parent_path(
            path,
            parent_path,
            sizeof(parent_path),
            name,
            sizeof(name))) {
        return 0;
    }

    if (!resolve_path(parent_path, &parent) || !parent.is_directory) {
        return 0;
    }

    if (!fat32_find_entry_location(parent.first_cluster, name, &loc)) {
        return 0;
    }

    if (!loc.found.is_directory) {
        return 0;
    }

    if (!fat32_directory_is_empty(loc.found.first_cluster)) {
        return 0;
    }

    if (!fat32_mark_entry_deleted(loc.sector_lba, loc.entry_index)) {
        return 0;
    }

    if (loc.found.first_cluster) {
        fat32_free_cluster_chain(loc.found.first_cluster);
    }

    return 1;
}