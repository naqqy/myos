#include "bootlog64.h"
#include "../k64_console.h"

int bootlog64_verbose(void) {
#if NAQ_BOOT_VERBOSE
    return 1;
#else
    return 0;
#endif
}

void bootlog64_print(const char* s) {
    if (bootlog64_verbose()) {
        k64_print(s);
    }
}

void bootlog64_print_hex(const char* label, nq_u64 value) {
    if (!bootlog64_verbose()) {
        return;
    }

    k64_print(label);
    k64_print_hex64(value);
    k64_print("\n");
}

void bootlog64_print_dec(const char* label, nq_u64 value) {
    if (!bootlog64_verbose()) {
        return;
    }

    k64_print(label);
    k64_print_dec64(value);
    k64_print("\n");
}