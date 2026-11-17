#include "usermode64.h"

#include "../../k64_console.h"
#include "../../mm/kmem64.h"
#include "../../mm/paging64.h"

/*
 * The original ring 3 proof of concept (usermode64_enter_test) lived here.
 * It ran test code from inside the kernel image by marking kernel pages
 * user-accessible in the shared page tables. It has been removed: user code
 * now only runs from ELF apps in the per-process user region
 * (see kernel64/exec/elf64.c and VMEM64_USER_BASE), entered through
 * usermode64_enter() in usermode64.asm.
 */
