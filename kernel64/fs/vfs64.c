#include "vfs64.h"
#include "fat32_64.h"
#include "initrd64.h"
#include "../k64_console.h"
#include "../safety/safe_mode64.h"
#include "../debug/bootlog64.h"

static int g_vfs_mounted = 0;
static vfs64_backend_t g_vfs_backend = VFS64_BACKEND_NONE;

static int g_initrd_mount_available = 0;
static int g_fat32_mount_available = 0;

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static int path_component_is_dot(const char* s, nq_u64 len) {
    return len == 1 && s[0] == '.';
}

static int path_component_is_dotdot(const char* s, nq_u64 len) {
    return len == 2 && s[0] == '.' && s[1] == '.';
}

static nq_u64 str_len64(const char* s) {
    nq_u64 len = 0;

    if (!s) {
        return 0;
    }

    while (s[len]) {
        len++;
    }

    return len;
}

static void str_copy64(char* dst, const char* src, nq_u64 dst_size) {
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

static int append_component(
    char* output,
    nq_u64 output_size,
    nq_u64* out_len,
    const char* component,
    nq_u64 component_len
) {
    nq_u64 len;

    if (!output || !out_len || !component || output_size == 0) {
        return 0;
    }

    len = *out_len;

    if (component_len == 0) {
        return 1;
    }

    /*
     * Need "/" + component + null.
     */
    if (len + 1 + component_len + 1 > output_size) {
        return 0;
    }

    if (len == 0) {
        output[len++] = '/';
    } else if (output[len - 1] != '/') {
        output[len++] = '/';
    }

    for (nq_u64 i = 0; i < component_len; i++) {
        output[len++] = component[i];
    }

    output[len] = 0;
    *out_len = len;

    return 1;
}

static void remove_last_component(char* output, nq_u64* out_len) {
    nq_u64 len;

    if (!output || !out_len) {
        return;
    }

    len = *out_len;

    if (len <= 1) {
        output[0] = '/';
        output[1] = 0;
        *out_len = 1;
        return;
    }

    /*
     * Remove trailing slash if present, except root.
     */
    if (len > 1 && output[len - 1] == '/') {
        len--;
    }

    while (len > 1 && output[len - 1] != '/') {
        len--;
    }

    if (len <= 1) {
        output[0] = '/';
        output[1] = 0;
        *out_len = 1;
        return;
    }

    output[len - 1] = 0;
    *out_len = len - 1;
}

static int str_starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return 0;
    }

    while (*prefix) {
        if (*s != *prefix) {
            return 0;
        }

        s++;
        prefix++;
    }

    return 1;
}

const char* vfs64_backend_name(vfs64_backend_t backend) {
    switch (backend) {
        case VFS64_BACKEND_FAT32:
            return "FAT32";

        case VFS64_BACKEND_INITRD:
            return "INITRD";

        case VFS64_BACKEND_NONE:
        default:
            return "NONE";
    }
}

vfs64_backend_t vfs64_root_backend(void) {
    return g_vfs_backend;
}

int vfs64_safe_mode_root_policy(void) {
    return safe_mode64_enabled();
}

static vfs64_backend_t vfs64_pick_backend_for_path(
    const char* normalized,
    const char** out_backend_path
) {
    if (out_backend_path) {
        *out_backend_path = normalized;
    }

    if (!normalized) {
        return VFS64_BACKEND_NONE;
    }

    /*
     * /initrd maps to INITRD if available.
     * /initrd/docs/readme.txt becomes /docs/readme.txt inside initrd.
     */
    if (str_starts_with(normalized, "/initrd")) {
        if (!initrd64_available()) {
            return VFS64_BACKEND_NONE;
        }

        const char* sub = normalized + 7;

        if (!sub[0]) {
            sub = "/";
        }

        if (out_backend_path) {
            *out_backend_path = sub;
        }

        return VFS64_BACKEND_INITRD;
    }

    /*
     * /disk maps to FAT32 only outside SAFE_HW mode.
     * In SAFE_HW, /disk is intentionally disabled for now.
     */
    if (str_starts_with(normalized, "/disk")) {
        if (safe_mode64_enabled()) {
            return VFS64_BACKEND_NONE;
        }

        if (!g_fat32_mount_available) {
            return VFS64_BACKEND_NONE;
        }

        const char* sub = normalized + 5;

        if (!sub[0]) {
            sub = "/";
        }

        if (out_backend_path) {
            *out_backend_path = sub;
        }

        return VFS64_BACKEND_FAT32;
    }

    return g_vfs_backend;
}

int vfs64_normalize_path(const char* input, char* output, nq_u64 output_size) {
    const char* p;
    nq_u64 out_len = 0;

    if (!output || output_size < 2) {
        return 0;
    }

    output[0] = 0;

    if (!input || input[0] == 0) {
        str_copy64(output, "/", output_size);
        return 1;
    }

    p = input;

    /*
     * Relative paths are treated as root-relative for now.
     */
    output[0] = '/';
    output[1] = 0;
    out_len = 1;

    while (*p) {
        const char* component_start;
        nq_u64 component_len = 0;

        while (is_sep(*p)) {
            p++;
        }

        if (*p == 0) {
            break;
        }

        component_start = p;

        while (*p && !is_sep(*p)) {
            component_len++;
            p++;
        }

        if (component_len == 0) {
            continue;
        }

        if (path_component_is_dot(component_start, component_len)) {
            continue;
        }

        if (path_component_is_dotdot(component_start, component_len)) {
            remove_last_component(output, &out_len);
            continue;
        }

        if (!append_component(
                output,
                output_size,
                &out_len,
                component_start,
                component_len)) {
            return 0;
        }
    }

    if (out_len == 0) {
        str_copy64(output, "/", output_size);
    }

    return 1;
}

int vfs64_mount_root(void) {
    g_vfs_mounted = 0;
    g_vfs_backend = VFS64_BACKEND_NONE;

    g_initrd_mount_available = initrd64_available();
    g_fat32_mount_available = 0;

    /*
     * SAFE_HW policy:
     *
     * Do not probe/mount unknown disks.
     * Root must come from INITRD.
     *
     * main.c should already skip block64_init() in SAFE_HW mode,
     * but this keeps VFS strict too.
     */
    if (safe_mode64_enabled()) {
        if (g_initrd_mount_available) {
            g_vfs_mounted = 1;
            g_vfs_backend = VFS64_BACKEND_INITRD;

            bootlog64_print("vfs64: SAFE_HW root policy active\n");
            bootlog64_print("vfs64: mounted / as INITRD\n");
            bootlog64_print("vfs64: /disk disabled in SAFE_HW mode\n");

            return 1;
        }

        k64_print("vfs64: SAFE_HW enabled but INITRD is unavailable\n");
        return 0;
    }

    /*
     * Dev/QEMU policy:
     *
     * Prefer FAT32 as root because it supports QEMU disk testing.
     * Fall back to INITRD if no disk is available.
     */
    if (fat32_64_init(0)) {
        g_vfs_mounted = 1;
        g_vfs_backend = VFS64_BACKEND_FAT32;
        g_fat32_mount_available = 1;

        bootlog64_print("vfs64: mounted / as FAT32\n");

        if (g_initrd_mount_available) {
            bootlog64_print("vfs64: INITRD available at /initrd policy path later\n");
        }

        return 1;
    }

    if (g_initrd_mount_available) {
        g_vfs_mounted = 1;
        g_vfs_backend = VFS64_BACKEND_INITRD;

        bootlog64_print("vfs64: mounted / as INITRD fallback\n");

        return 1;
    }

    return 0;
}

int vfs64_mounted(void) {
    return g_vfs_mounted;
}

void vfs64_print_summary(void) {
    k64_print("VFS64:\n");

    k64_print("  mounted: ");
    k64_print(g_vfs_mounted ? "yes\n" : "no\n");

    k64_print("  root backend: ");
    k64_print(vfs64_backend_name(g_vfs_backend));
    k64_print("\n");

    k64_print("  policy: ");
    k64_print(safe_mode64_enabled() ? "SAFE_HW\n" : "DEV/QEMU\n");

    k64_print("  /initrd: ");
    k64_print(initrd64_available() ? "available\n" : "unavailable\n");

    k64_print("  /disk: ");
    if (safe_mode64_enabled()) {
        k64_print("disabled by SAFE_HW\n");
    } else {
        k64_print(g_fat32_mount_available ? "FAT32 available\n" : "unavailable\n");
    }
}

int vfs64_list_dir(const char* path) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;
    vfs64_backend_t backend;

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    backend = vfs64_pick_backend_for_path(normalized, &backend_path);

    if (backend == VFS64_BACKEND_INITRD) {
        return initrd64_list_path(backend_path);
    }

    if (backend == VFS64_BACKEND_FAT32) {
        return fat32_64_list_path(backend_path);
    }

    if (safe_mode64_enabled() && str_starts_with(normalized, "/disk")) {
        k64_print("vfs64: /disk is disabled in SAFE_HW mode\n");
    } else {
        k64_print("vfs64: no backend for path\n");
    }

    return 0;
}

int vfs64_read_file(
    const char* path,
    void* out_buf,
    nq_u64 max_bytes,
    nq_u64* out_size
) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;
    vfs64_backend_t backend;

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    backend = vfs64_pick_backend_for_path(normalized, &backend_path);

    if (backend == VFS64_BACKEND_INITRD) {
        return initrd64_read_file(
            backend_path,
            out_buf,
            max_bytes,
            out_size
        );
    }

    if (backend == VFS64_BACKEND_FAT32) {
        return fat32_64_read_file_path(
            backend_path,
            (nq_u8*)out_buf,
            max_bytes,
            out_size
        );
    }

    if (safe_mode64_enabled() && str_starts_with(normalized, "/disk")) {
        k64_print("vfs64: /disk is disabled in SAFE_HW mode\n");
    } else {
        k64_print("vfs64: no backend for path\n");
    }

    return 0;
}

int vfs64_write_file(
    const char* path,
    const void* data,
    nq_u64 size
) {
    if (safe_mode64_enabled()) {
        k64_print("vfs64: write blocked by SAFE_HW mount policy\n");
        return 0;
    }

    if (g_vfs_backend != VFS64_BACKEND_FAT32) {
        k64_print("vfs64: root filesystem is read-only\n");
        return 0;
    }

    char normalized[VFS64_MAX_PATH];

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (size > 0 && !data) {
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    return fat32_64_write_file_path(
        normalized,
        (const nq_u8*)data,
        size
    );
}

int vfs64_mkdir(const char* path) {
    if (safe_mode64_enabled()) {
        k64_print("vfs64: write blocked by SAFE_HW mount policy\n");
        return 0;
    }

    if (g_vfs_backend != VFS64_BACKEND_FAT32) {
        k64_print("vfs64: root filesystem is read-only\n");
        return 0;
    }

    char normalized[VFS64_MAX_PATH];

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    return fat32_64_mkdir_path(normalized);
}

int vfs64_rm(const char* path) {
    if (safe_mode64_enabled()) {
        k64_print("vfs64: write blocked by SAFE_HW mount policy\n");
        return 0;
    }

    if (g_vfs_backend != VFS64_BACKEND_FAT32) {
        k64_print("vfs64: root filesystem is read-only\n");
        return 0;
    }

    char normalized[VFS64_MAX_PATH];

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    return fat32_64_rm_path(normalized);
}

int vfs64_rmdir(const char* path) {
    if (safe_mode64_enabled()) {
        k64_print("vfs64: write blocked by SAFE_HW mount policy\n");
        return 0;
    }

    if (g_vfs_backend != VFS64_BACKEND_FAT32) {
        k64_print("vfs64: root filesystem is read-only\n");
        return 0;
    }

    char normalized[VFS64_MAX_PATH];

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    return fat32_64_rmdir_path(normalized);
}

int vfs64_stat(const char* path, vfs64_stat_t* out) {
    static nq_u8 tmp[1];
    nq_u64 size = 0;

    if (!out) {
        return 0;
    }

    out->size = 0;
    out->type = VFS64_TYPE_UNKNOWN;

    /*
     * POC:
     * Try reading with zero/one byte buffer.
     * Better later: backend-specific stat.
     */
    if (vfs64_read_file(path, tmp, 1, &size)) {
        out->size = size;
        out->type = VFS64_TYPE_FILE;
        return 1;
    }

    return 0;
}