#include "naq_boot_info.h"
#include "../k64_console.h"

#if defined(__x86_64__)
#define STACK_CHK_HALT() \
    do { __asm__ volatile ("cli; hlt"); } while (1)
#else
#define STACK_CHK_HALT() \
    do { for (;;) {} } while (1)
#endif

/*
 * GCC stack protector expects this global symbol.
 *
 * Later we can seed this from RDRAND, timer jitter, boot time, or memory-map
 * noise. For now, a fixed nonzero value is enough to enable stack-smash
 * detection and fix the linker errors.
 */
nq_u64 __stack_chk_guard = 0x595E9FBD94FDA766ull;

/*
 * IMPORTANT:
 * Compile this file with -fno-stack-protector.
 * If this function gets stack-protected too, a stack failure can recurse.
 */
__attribute__((noreturn))
void __stack_chk_fail(void) {
    k64_print("\n");
    k64_print("************************************************\n");
    k64_print("*** KERNEL PANIC: STACK SMASHING DETECTED    ***\n");
    k64_print("************************************************\n");
    k64_print("A stack canary check failed. System halted.\n");

    for (;;) {
        STACK_CHK_HALT();
    }
}

/*
 * Optional boot-time initializer.
 * Call this early from kernel64_main() later if you want to randomize it.
 */
void stack_chk64_init(nq_u64 seed) {
    if (seed == 0) {
        seed = 0x595E9FBD94FDA766ull;
    }

    /*
     * Avoid common bad canary values.
     */
    if (seed == 0 || seed == 0xFFFFFFFFFFFFFFFFull) {
        seed ^= 0xA5A5D3C17BADC0DEull;
    }

    __stack_chk_guard = seed;
}