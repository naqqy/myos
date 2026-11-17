#include "vfs64.h"
#include "fat32_64.h"
#include "initrd64.h"
#include "../k64_console.h"
#include "../safety/safe_mode64.h"
#include "../debug/bootlog64.h"
#include "../mm/kmem64.h"
#include "../storage/block64.h"

static int g_vfs_mounted = 0;
static vfs64_backend_t g_vfs_backend = VFS64_BACKEND_NONE;

static int g_initrd_mount_available = 0;
static int g_fat32_mount_available = 0;

/*
 * 1 while a USB disk's FAT32 filesystem is mounted at /usb.
 */
static volatile int g_usb_mounted = 0;
static nq_u32 g_usb_block_id = 0;

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
     * /usb is the USB disk. Unlike /disk it is allowed in SAFE_HW mode:
     * it is removable media the user plugged in, not an internal drive.
     */
    if (str_starts_with(normalized, "/usb") &&
        (normalized[4] == 0 || normalized[4] == '/')) {
        if (!g_usb_mounted) {
            return VFS64_BACKEND_NONE;
        }

        const char* sub = normalized + 4;

        if (!sub[0]) {
            sub = "/";
        }

        if (out_backend_path) {
            *out_backend_path = sub;
        }

        return VFS64_BACKEND_FAT32;
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

int vfs64_usb_mount(nq_u32 block_device_id, nq_u64 base_lba) {
    /*
     * The FAT32 driver handles one filesystem at a time.
     */
    if (g_usb_mounted || g_vfs_backend == VFS64_BACKEND_FAT32) {
        return 0;
    }

    if (!fat32_64_init_device(block_device_id, base_lba)) {
        return 0;
    }

    g_usb_block_id = block_device_id;
    g_usb_mounted = 1;

    return 1;
}

void vfs64_usb_unmount(void) {
    if (g_usb_mounted) {
        g_usb_mounted = 0;
        fat32_64_unmount();
    }
}

int vfs64_usb_mounted(void) {
    return g_usb_mounted;
}

int vfs64_write_locked(const char* path) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;

    if (!path || !vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        return 0;
    }

    if (vfs64_pick_backend_for_path(normalized, &backend_path) != VFS64_BACKEND_FAT32) {
        return 0;
    }

    /*
     * A stripped prefix means /usb; otherwise FAT32 is the root, on the
     * boot block device.
     */
    if (backend_path != normalized) {
        return !block64_writes_allowed(g_usb_block_id);
    }

    const block64_device_t* boot = block64_boot_device();

    return !boot || !block64_writes_allowed(boot->id);
}

static int path_is_root(const char* normalized) {
    return normalized[0] == '/' && normalized[1] == 0;
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
        int listed = initrd64_list_path(backend_path);

        if (listed && g_usb_mounted && path_is_root(normalized)) {
            k64_print("usb/   (USB disk)\n");
        }

        return listed;
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

/*
 * Resolves a path for a write operation. Only FAT32 is writable: the
 * root filesystem when it is FAT32 (dev/QEMU mode), or the USB disk at
 * /usb. The block layer still refuses the write until disk writes have
 * been unlocked ("diskunlock confirm").
 */
static int vfs64_resolve_writable(
    const char* path,
    char* normalized,
    nq_u64 normalized_size,
    const char** out_backend_path
) {
    if (!g_vfs_mounted) {
        k64_print("vfs64: root filesystem not mounted\n");
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, normalized_size)) {
        k64_print("vfs64: invalid or too-long path\n");
        return 0;
    }

    vfs64_backend_t backend = vfs64_pick_backend_for_path(normalized, out_backend_path);

    if (backend != VFS64_BACKEND_FAT32) {
        if (g_usb_mounted) {
            k64_print("vfs64: this location is read-only; files can be written under /usb\n");
        } else {
            k64_print("vfs64: this location is read-only\n");
        }

        return 0;
    }

    /*
     * FAT32 as the root filesystem means an internal or emulated disk,
     * which SAFE_HW never writes to.
     */
    if (safe_mode64_enabled() && *out_backend_path == normalized) {
        k64_print("vfs64: write blocked by SAFE_HW mount policy\n");
        return 0;
    }

    return 1;
}

int vfs64_write_file(
    const char* path,
    const void* data,
    nq_u64 size
) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;

    if (size > 0 && !data) {
        return 0;
    }

    if (!vfs64_resolve_writable(path, normalized, sizeof(normalized), &backend_path)) {
        return 0;
    }

    return fat32_64_write_file_path(
        backend_path,
        (const nq_u8*)data,
        size
    );
}

int vfs64_mkdir(const char* path) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;

    if (!vfs64_resolve_writable(path, normalized, sizeof(normalized), &backend_path)) {
        return 0;
    }

    return fat32_64_mkdir_path(backend_path);
}

int vfs64_rm(const char* path) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;

    if (!vfs64_resolve_writable(path, normalized, sizeof(normalized), &backend_path)) {
        return 0;
    }

    return fat32_64_rm_path(backend_path);
}

int vfs64_rmdir(const char* path) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;

    if (!vfs64_resolve_writable(path, normalized, sizeof(normalized), &backend_path)) {
        return 0;
    }

    return fat32_64_rmdir_path(backend_path);
}

int vfs64_stat(const char* path, vfs64_stat_t* out) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;
    vfs64_backend_t backend;
    nq_u64 size = 0;
    int is_dir = 0;

    if (!out) {
        return 0;
    }

    out->size = 0;
    out->type = VFS64_TYPE_UNKNOWN;

    if (!g_vfs_mounted || !path) {
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        return 0;
    }

    /*
     * Ask the backend's directory/index for the size instead of reading the
     * file, so stat works for files of any size.
     */
    backend = vfs64_pick_backend_for_path(normalized, &backend_path);

    if (backend == VFS64_BACKEND_FAT32 && path_is_root(backend_path)) {
        out->type = VFS64_TYPE_DIR;
        return 1;
    }

    if (backend == VFS64_BACKEND_INITRD) {
        if (!initrd64_file_size(backend_path, &size)) {
            return 0;
        }
    } else if (backend == VFS64_BACKEND_FAT32) {
        if (!fat32_64_stat_path(backend_path, &size, &is_dir)) {
            return 0;
        }
    } else {
        return 0;
    }

    out->size = size;
    out->type = is_dir ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;

    return 1;
}

int vfs64_enum_dir(const char* path, vfs64_enum_cb_t cb, void* ctx) {
    char normalized[VFS64_MAX_PATH];
    const char* backend_path = 0;
    vfs64_backend_t backend;

    if (!g_vfs_mounted || !path || !cb) {
        return 0;
    }

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        return 0;
    }

    backend = vfs64_pick_backend_for_path(normalized, &backend_path);

    if (backend == VFS64_BACKEND_INITRD) {
        int listed = initrd64_enum_path(backend_path, cb, ctx);

        if (listed && g_usb_mounted && path_is_root(normalized)) {
            cb("usb", 0, 1, ctx);
        }

        return listed;
    }

    if (backend == VFS64_BACKEND_FAT32) {
        return fat32_64_enum_path(backend_path, cb, ctx);
    }

    return 0;
}

int vfs64_read_file_alloc(
    const char* path,
    nq_u64 max_bytes,
    void** out_buf,
    nq_u64* out_size
) {
    vfs64_stat_t st;
    nq_u64 size = 0;

    if (!out_buf || !out_size) {
        return 0;
    }

    *out_buf = 0;
    *out_size = 0;

    if (!vfs64_stat(path, &st) || st.type != VFS64_TYPE_FILE) {
        return 0;
    }

    if (st.size > max_bytes) {
        return 0;
    }

    /*
     * Zero-length files still get a valid (1 byte) buffer so callers can
     * treat a non-null pointer as "file is open".
     */
    void* buf = kmalloc64(st.size ? st.size : 1);

    if (!buf) {
        return 0;
    }

    if (st.size > 0 && !vfs64_read_file(path, buf, st.size, &size)) {
        kfree64(buf);
        return 0;
    }

    *out_buf = buf;
    *out_size = size;

    return 1;
}