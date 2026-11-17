#include "include/naqos.h"

/*
 * CRASH.ELF
 *
 * Deliberately faults so the kernel's user-fault handling can be tested:
 * the kernel should report the exception, kill only this process, and keep
 * the shell and every other app running.
 *
 * Usage from the shell:
 *   run crash
 */

int main(void) {
    puts("crash: about to write through a null pointer\n");

    /*
     * volatile so the compiler cannot remove or reorder the bad write.
     */
    volatile u32* bad = (volatile u32*)0;

    *bad = 0xDEADBEEFu;

    puts("crash: ERROR - the write did not fault\n");

    return 1;
}
