#include "vfs64.h"
#include "fat32_64.h"
#include "initrd64.h"
#include "../k64_console.h"

static int g_vfs_mounted = 0;

typedef enum vfs64_backend {
    VFS64_BACKEND_NONE = 0,
    VFS64_BACKEND_FAT32,
    VFS64_BACKEND_INITRD
} vfs64_backend_t;

static vfs64_backend_t g_vfs_backend = VFS64_BACKEND_NONE;

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

    if (fat32_64_init(0)) {
        g_vfs_mounted = 1;
        g_vfs_backend = VFS64_BACKEND_FAT32;
        return 1;
    }

    if (initrd64_available()) {
        g_vfs_mounted = 1;
        g_vfs_backend = VFS64_BACKEND_INITRD;
        return 1;
    }

    return 0;
}

int vfs64_mounted(void) {
    return g_vfs_mounted;
}

void vfs64_print_summary(void) {
    k64_print("VFS64: ");

    if (!g_vfs_mounted) {
        k64_print("not mounted\n");
        return;
    }

    k64_print("root mounted\n");
    k64_print("Root backend: FAT32 read-only\n");
    k64_print("Path normalization: enabled\n");

    fat32_64_print_summary();
}

int vfs64_list_dir(const char* path) {
    char normalized[VFS64_MAX_PATH];

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    if (g_vfs_backend == VFS64_BACKEND_FAT32) {
        return fat32_64_list_path(normalized);
    }

    if (g_vfs_backend == VFS64_BACKEND_INITRD) {
        return initrd64_list_path(normalized);
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

    if (out_size) {
        *out_size = 0;
    }

    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!out_buf || max_bytes == 0) {
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    if (g_vfs_backend == VFS64_BACKEND_FAT32) {
        return fat32_64_read_file_path(
            normalized,
            (nq_u8*)out_buf,
            max_bytes,
            out_size
        );
    }

    if (g_vfs_backend == VFS64_BACKEND_INITRD) {
        return initrd64_read_file(
            normalized,
            out_buf,
            max_bytes,
            out_size
        );
    }

    return 0;
}

int vfs64_write_file(
    const char* path,
    const void* data,
    nq_u64 size
) {
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