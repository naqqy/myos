#ifndef ELF64_H
#define ELF64_H

#include "naq_boot_info.h"
#include "../proc/process64.h"

#define ELF64_MAX_IMAGE_SIZE   (512u * 1024u)
#define ELF64_USER_STACK_SIZE  (64u * 1024u)

int elf64_exec_file(const char* path);

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
