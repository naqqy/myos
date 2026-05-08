#ifndef KMEM64_H
#define KMEM64_H

#include "naq_boot_info.h"

int   kmem64_init(nq_u64 initial_pages);
void* kmalloc64(nq_u64 size);
void  kfree64(void* ptr);

nq_u64 kmem64_total_bytes(void);
nq_u64 kmem64_used_bytes(void);
nq_u64 kmem64_free_bytes(void);

void kmem64_print_summary(void);

#endif