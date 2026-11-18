#ifndef ELF64_H
#define ELF64_H

#include "naq_boot_info.h"
#include "../proc/process64.h"

#define ELF64_MAX_IMAGE_SIZE  (16ull * 1024ull * 1024ull)
#define ELF64_USER_STACK_SIZE (4ull * 1024ull * 1024ull)

int elf64_exec_file(const char* path);

/*
 * Per-process address space note:
 * ELF apps can still be linked at the same fixed virtual addresses, but each
 * app now runs under its own PML4/CR3 so their code/data/stack pages do not
 * overwrite each other.
 */

/*
 * Spawn an ELF app as a process64 task.
 *
 * elf64_spawn_file() keeps the old/background behavior:
 *   private terminal, visible with view <pid>
 *
 * elf64_spawn_file_ex() allows shell run/spawn to choose:
 *   PROCESS64_SPAWN_FOREGROUND       -> share current KER64 terminal
 *   PROCESS64_SPAWN_PRIVATE_TERMINAL -> private process terminal
 */
int elf64_spawn_file(const char* path);
int elf64_spawn_file_ex(const char* path, nq_u32 flags);

#endif
