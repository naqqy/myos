#include "initrd64.h"
#include "../k64_console.h"

#define INITRD64_MAGIC   0x4452494Eu
#define INITRD64_VERSION 1u
#define INITRD64_MAX_PATH 128u

typedef struct initrd64_header {
    nq_u32 magic;
    nq_u32 version;
    nq_u32 file_count;
    nq_u32 header_size;
} __attribute__((packed)) initrd64_header_t;

typedef struct initrd64_entry {
    char path[INITRD64_MAX_PATH];
    nq_u64 offset;
    nq_u64 size;
} __attribute__((packed)) initrd64_entry_t;

static const nq_u8* g_initrd_base = 0;
static nq_u64 g_initrd_size = 0;
static const initrd64_header_t* g_header = 0;
static const initrd64_entry_t* g_entries = 0;
static int g_initrd_available = 0;

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

static int str_starts_with_ci(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return 0;
    }

    while (*prefix) {
        if (ascii_lower(*s) != ascii_lower(*prefix)) {
            return 0;
        }

        s++;
        prefix++;
    }

    return 1;
}

int initrd64_init(nq_u64 base, nq_u64 size) {
    g_initrd_available = 0;
    g_initrd_base = 0;
    g_initrd_size = 0;
    g_header = 0;
    g_entries = 0;

    if (!base || size < sizeof(initrd64_header_t)) {
        return 0;
    }

    const initrd64_header_t* h = (const initrd64_header_t*)base;

    if (h->magic != INITRD64_MAGIC ||
        h->version != INITRD64_VERSION ||
        h->header_size < sizeof(initrd64_header_t)) {
        return 0;
    }

    nq_u64 entries_size = (nq_u64)h->file_count * sizeof(initrd64_entry_t);
    nq_u64 table_end = (nq_u64)h->header_size + entries_size;

    if (table_end > size) {
        return 0;
    }

    g_initrd_base = (const nq_u8*)base;
    g_initrd_size = size;
    g_header = h;
    g_entries = (const initrd64_entry_t*)(g_initrd_base + h->header_size);
    g_initrd_available = 1;

    return 1;
}

int initrd64_available(void) {
    return g_initrd_available;
}

void initrd64_print_summary(void) {
    k64_print("INITRD64: ");

    if (!g_initrd_available) {
        k64_print("not available\n");
        return;
    }

    k64_print("available\n");

    k64_print("Files: ");
    k64_print_dec64(g_header->file_count);
    k64_print("\n");

    k64_print("Size: ");
    k64_print_dec64(g_initrd_size);
    k64_print(" bytes\n");
}

static const initrd64_entry_t* initrd64_find_file(const char* path) {
    if (!g_initrd_available || !path) {
        return 0;
    }

    for (nq_u32 i = 0; i < g_header->file_count; i++) {
        if (str_eq_ci(g_entries[i].path, path)) {
            return &g_entries[i];
        }
    }

    return 0;
}

int initrd64_list_path(const char* path) {
    if (!g_initrd_available) {
        k64_print("initrd: not available\n");
        return 0;
    }

    if (!path || !path[0]) {
        path = "/";
    }

    k64_print("Directory: ");
    k64_print(path);
    k64_print("\n");

    for (nq_u32 i = 0; i < g_header->file_count; i++) {
        const char* p = g_entries[i].path;

        if (str_eq_ci(path, "/")) {
            k64_print("  ");
            k64_print(p);
            k64_print("  size=");
            k64_print_dec64(g_entries[i].size);
            k64_print("\n");
        } else if (str_starts_with_ci(p, path)) {
            k64_print("  ");
            k64_print(p);
            k64_print("  size=");
            k64_print_dec64(g_entries[i].size);
            k64_print("\n");
        }
    }

    return 1;
}

int initrd64_read_file(
    const char* path,
    void* out_buf,
    nq_u64 max_bytes,
    nq_u64* out_size
) {
    if (out_size) {
        *out_size = 0;
    }

    if (!g_initrd_available || !path || !out_buf) {
        return 0;
    }

    const initrd64_entry_t* e = initrd64_find_file(path);

    if (!e) {
        return 0;
    }

    if (e->offset + e->size > g_initrd_size) {
        return 0;
    }

    if (e->size > max_bytes) {
        return 0;
    }

    nq_u8* dst = (nq_u8*)out_buf;
    const nq_u8* src = g_initrd_base + e->offset;

    for (nq_u64 i = 0; i < e->size; i++) {
        dst[i] = src[i];
    }

    if (out_size) {
        *out_size = e->size;
    }

    return 1;
}