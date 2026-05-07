#ifndef KMEM_H
#define KMEM_H

typedef unsigned int u32;

void kmem_init(void* heap_start, u32 heap_size);
void* kmalloc(u32 size);
void  kfree(void* ptr);

u32 kmem_committed_size(void);
u32 kmem_max_size(void);

#endif