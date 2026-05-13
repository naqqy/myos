#include "kmem64.h"
#include "pmm64.h"
#include "../k64_console.h"
#include "vmem64.h"

#define KMEM64_ALIGN 16ull
#define KMEM64_MIN_EXPAND_PAGES 4ull

typedef struct kmem64_block {
    nq_u64 size;
    nq_u64 free;
    struct kmem64_block* next;
} kmem64_block_t;

static kmem64_block_t* g_heap_head = 0;
static nq_u64 g_heap_total_bytes = 0;
static nq_u64 g_heap_used_bytes = 0;
static int g_heap_ready = 0;

static nq_u64 align_up64(nq_u64 value, nq_u64 align) {
    nq_u64 mask = align - 1ull;

    if ((value & mask) == 0) {
        return value;
    }

    return (value + mask) & ~mask;
}

static nq_u64 header_size(void) {
    return align_up64((nq_u64)sizeof(kmem64_block_t), KMEM64_ALIGN);
}

static void mem_zero(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static kmem64_block_t* block_from_payload(void* ptr) {
    if (!ptr) {
        return 0;
    }

    return (kmem64_block_t*)((nq_u8*)ptr - header_size());
}

static void* payload_from_block(kmem64_block_t* block) {
    if (!block) {
        return 0;
    }

    return (void*)((nq_u8*)block + header_size());
}

static kmem64_block_t* find_last_block(void) {
    kmem64_block_t* current = g_heap_head;

    if (!current) {
        return 0;
    }

    while (current->next) {
        current = current->next;
    }

    return current;
}

static void split_block(kmem64_block_t* block, nq_u64 size) {
    nq_u64 hsize = header_size();

    if (!block) {
        return;
    }

    if (block->size < size + hsize + KMEM64_ALIGN) {
        return;
    }

    kmem64_block_t* new_block =
        (kmem64_block_t*)((nq_u8*)payload_from_block(block) + size);

    new_block->size = block->size - size - hsize;
    new_block->free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
}

static int blocks_are_adjacent(kmem64_block_t* a, kmem64_block_t* b) {
    if (!a || !b) {
        return 0;
    }

    nq_u8* a_end = (nq_u8*)payload_from_block(a) + a->size;

    return a_end == (nq_u8*)b;
}

static void coalesce_blocks(void) {
    kmem64_block_t* current = g_heap_head;
    nq_u64 hsize = header_size();

    while (current && current->next) {
        if (current->free &&
            current->next->free &&
            blocks_are_adjacent(current, current->next)) {
            current->size += hsize + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

static int kmem64_expand(nq_u64 min_payload_bytes) {
    nq_u64 hsize = header_size();
    nq_u64 needed = min_payload_bytes + hsize;
    nq_u64 pages = align_up64(needed, PMM64_FRAME_SIZE) / PMM64_FRAME_SIZE;

    if (pages < KMEM64_MIN_EXPAND_PAGES) {
        pages = KMEM64_MIN_EXPAND_PAGES;
    }

    nq_u64 phys = pmm64_alloc_contiguous_frames(pages);

    if (!phys) {
        return 0;
    }

    nq_u64 bytes = pages * PMM64_FRAME_SIZE;

    /*
    * New model:
    * PMM returns physical frames.
    * Kernel accesses them through the higher-half direct map.
    */
    kmem64_block_t* block = (kmem64_block_t*)vmem64_phys_to_ptr(phys);

    mem_zero(block, bytes);

    block->size = bytes - hsize;
    block->free = 1;
    block->next = 0;

    if (!g_heap_head) {
        g_heap_head = block;
    } else {
        kmem64_block_t* last = find_last_block();

        if (last) {
            last->next = block;
        }
    }

    g_heap_total_bytes += block->size;

    coalesce_blocks();

    return 1;
}

int kmem64_init(nq_u64 initial_pages) {
    g_heap_head = 0;
    g_heap_total_bytes = 0;
    g_heap_used_bytes = 0;
    g_heap_ready = 0;

    if (initial_pages == 0) {
        initial_pages = KMEM64_MIN_EXPAND_PAGES;
    }

    if (!kmem64_expand(initial_pages * PMM64_FRAME_SIZE)) {
        return 0;
    }

    g_heap_ready = 1;

    return 1;
}

void* kmalloc64(nq_u64 size) {
    if (!g_heap_ready || size == 0) {
        return 0;
    }

    size = align_up64(size, KMEM64_ALIGN);

    for (;;) {
        kmem64_block_t* current = g_heap_head;

        while (current) {
            if (current->free && current->size >= size) {
                split_block(current, size);

                current->free = 0;
                g_heap_used_bytes += current->size;

                return payload_from_block(current);
            }

            current = current->next;
        }

        if (!kmem64_expand(size)) {
            return 0;
        }
    }
}

void kfree64(void* ptr) {
    if (!g_heap_ready || !ptr) {
        return;
    }

    kmem64_block_t* block = block_from_payload(ptr);

    if (!block || block->free) {
        return;
    }

    block->free = 1;

    if (g_heap_used_bytes >= block->size) {
        g_heap_used_bytes -= block->size;
    } else {
        g_heap_used_bytes = 0;
    }

    coalesce_blocks();
}

nq_u64 kmem64_total_bytes(void) {
    return g_heap_total_bytes;
}

nq_u64 kmem64_used_bytes(void) {
    return g_heap_used_bytes;
}

nq_u64 kmem64_free_bytes(void) {
    if (g_heap_total_bytes < g_heap_used_bytes) {
        return 0;
    }

    return g_heap_total_bytes - g_heap_used_bytes;
}

void kmem64_print_summary(void) {
    k64_print("KMEM64 initialized.\n");

    k64_print("Heap total: ");
    k64_print_dec64(kmem64_total_bytes());
    k64_print(" bytes\n");

    k64_print("Heap used: ");
    k64_print_dec64(kmem64_used_bytes());
    k64_print(" bytes\n");

    k64_print("Heap free: ");
    k64_print_dec64(kmem64_free_bytes());
    k64_print(" bytes\n");
}
