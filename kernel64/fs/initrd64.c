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

int initrd64_enum_path(const char* path, initrd64_enum_cb_t cb, void* ctx) {
    if (!g_initrd_available || !cb) {
        return 0;
    }

    if (!path || !path[0]) {
        path = "/";
    }

    nq_u64 prefix_len = 0;

    while (path[prefix_len]) {
        prefix_len++;
    }

    /*
     * Compare without a trailing slash so "/apps" and "/apps/" both work.
     */
    while (prefix_len > 1 && path[prefix_len - 1] == '/') {
        prefix_len--;
    }

    if (prefix_len == 1 && path[0] == '/') {
        prefix_len = 0;
    }

    for (nq_u32 i = 0; i < g_header->file_count; i++) {
        const char* p = g_entries[i].path;
        nq_u64 k = 0;
        int match = 1;

        for (k = 0; k < prefix_len; k++) {
            char a = p[k];
            char b = path[k];

            if (a >= 'a' && a <= 'z') {
                a = (char)(a - 'a' + 'A');
            }

            if (b >= 'a' && b <= 'z') {
                b = (char)(b - 'a' + 'A');
            }

            if (!a || a != b) {
                match = 0;
                break;
            }
        }

        if (!match || p[prefix_len] != '/') {
            continue;
        }

        const char* name = p + prefix_len + 1;
        nq_u64 component_len = 0;
        int nested = 0;

        for (nq_u64 j = 0; name[j]; j++) {
            if (name[j] == '/') {
                nested = 1;
                break;
            }

            component_len++;
        }

        if (!name[0]) {
            continue;
        }

        if (!nested) {
            cb(name, g_entries[i].size, 0, ctx);
            continue;
        }

        /*
         * The file is deeper down, so its first component is a
         * subdirectory of `path`. The INITRD has no directory records;
         * report each subdirectory once, when its first file is seen.
         */
        char dir_name[INITRD64_MAX_PATH];
        int seen = 0;

        if (component_len == 0 || component_len >= sizeof(dir_name)) {
            continue;
        }

        for (nq_u64 j = 0; j < component_len; j++) {
            dir_name[j] = name[j];
        }

        dir_name[component_len] = 0;

        for (nq_u32 earlier = 0; earlier < i && !seen; earlier++) {
            const char* q = g_entries[earlier].path;
            nq_u64 k2 = 0;

            /*
             * Same directory if the earlier path matches up to and
             * including this component and its trailing slash.
             */
            for (k2 = 0; k2 <= prefix_len + component_len + 1; k2++) {
                char a = q[k2];
                char b = p[k2];

                if (a >= 'a' && a <= 'z') {
                    a = (char)(a - 'a' + 'A');
                }

                if (b >= 'a' && b <= 'z') {
                    b = (char)(b - 'a' + 'A');
                }

                if (!a || a != b) {
                    break;
                }
            }

            if (k2 > prefix_len + component_len + 1) {
                seen = 1;
            }
        }

        if (!seen) {
            cb(dir_name, 0, 1, ctx);
        }
    }

    return 1;
}

int initrd64_file_size(const char* path, nq_u64* out_size) {
    const initrd64_entry_t* e = initrd64_find_file(path);

    if (!e) {
        return 0;
    }

    if (out_size) {
        *out_size = e->size;
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