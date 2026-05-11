#ifndef ELF64_H
#define ELF64_H

#include "naq_boot_info.h"

#define ELF64_MAX_IMAGE_SIZE   (512u * 1024u)
#define ELF64_USER_STACK_SIZE  (64u * 1024u)

int elf64_exec_file(const char* path);
int elf64_spawn_file(const char* path);

#endif