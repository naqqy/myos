#include "safe_mode64.h"
#include "../k64_console.h"

static int g_safe_hw_enabled = NAQ_SAFE_HW_DEFAULT;
static int g_disk_writes_unlocked = 0;

void safe_mode64_init(void) {
#if NAQ_SAFE_HW_DEFAULT
    g_safe_hw_enabled = 1;
#else
    g_safe_hw_enabled = 0;
#endif

    g_disk_writes_unlocked = 0;
}

int safe_mode64_enabled(void) {
    return g_safe_hw_enabled;
}

int safe_mode64_disk_writes_allowed(void) {
    if (!g_safe_hw_enabled) {
        return 1;
    }

    return g_disk_writes_unlocked;
}

void safe_mode64_unlock_disk_writes(void) {
    g_disk_writes_unlocked = 1;
}

void safe_mode64_lock_disk_writes(void) {
    g_disk_writes_unlocked = 0;
}

void safe_mode64_print_status(void) {
    k64_print("SAFE_HW: ");

    if (g_safe_hw_enabled) {
        k64_print("ON\n");
        k64_print("  disk writes: ");
        k64_print(g_disk_writes_unlocked ? "UNLOCKED\n" : "LOCKED\n");
        k64_print("  root policy: prefer INITRD\n");
    } else {
        k64_print("OFF\n");
        k64_print("  disk writes: allowed\n");
        k64_print("  root policy: normal/dev\n");
    }
}