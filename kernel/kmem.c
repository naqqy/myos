#include "kmem.h"
#include "paging.h"
#include "pmm.h"

typedef unsigned char u8;

typedef struct block_header {
    u32 size;
    u32 free;
    struct block_header* next;
} block_header_t;

#define KMEM_ALIGN 8
#define KMEM_PAGE_SIZE 4096
#define BLOCK_HEADER_SIZE ((u32)sizeof(block_header_t))
#define KMEM_MIN_COMMITTED KMEM_PAGE_SIZE

static block_header_t* heap_head = 0;
static u8* heap_start_ptr = 0;
static u32 heap_total_size = 0;
static u32 heap_committed = 0;

static u32 align_up(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}

static u32 align_down(u32 value, u32 align) {
    return value & ~(align - 1);
}

static void mem_zero(void* ptr, u32 size) {
    u8* p = (u8*)ptr;
    for (u32 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static block_header_t* find_last_block(void) {
    block_header_t* current = heap_head;
    if (!current) {
        return 0;
    }

    while (current->next) {
        current = current->next;
    }

    return current;
}

static int kmem_expand(u32 min_bytes) {
    if (heap_committed >= heap_total_size) {
        return 0;
    }

    u32 bytes = align_up(min_bytes, KMEM_PAGE_SIZE);
    if (bytes == 0) {
        bytes = KMEM_PAGE_SIZE;
    }

    if (heap_committed + bytes > heap_total_size) {
        bytes = heap_total_size - heap_committed;
        bytes = align_down(bytes, KMEM_PAGE_SIZE);
    }

    if (bytes == 0) {
        return 0;
    }

    u32 old_committed = heap_committed;

    for (u32 off = 0; off < bytes; off += KMEM_PAGE_SIZE) {
        u32 frame = pmm_alloc_frame();
        if (!frame) {
            return 0;
        }

        u32 virt = (u32)(heap_start_ptr + heap_committed + off);
        paging_map_page(virt, frame, PAGE_WRITABLE);
        mem_zero((void*)virt, KMEM_PAGE_SIZE);
    }

    heap_committed += bytes;

    if (!heap_head) {
        heap_head = (block_header_t*)heap_start_ptr;
        heap_head->size = bytes - BLOCK_HEADER_SIZE;
        heap_head->free = 1;
        heap_head->next = 0;
        return 1;
    }

    block_header_t* last = find_last_block();
    block_header_t* new_block = (block_header_t*)(heap_start_ptr + old_committed);

    new_block->size = bytes - BLOCK_HEADER_SIZE;
    new_block->free = 1;
    new_block->next = 0;

    if (last->free) {
        last->size += BLOCK_HEADER_SIZE + new_block->size;
    } else {
        last->next = new_block;
    }

    return 1;
}

static void split_block(block_header_t* block, u32 size) {
    if (block->size < size + BLOCK_HEADER_SIZE + KMEM_ALIGN) {
        return;
    }

    block_header_t* new_block =
        (block_header_t*)((u8*)block + BLOCK_HEADER_SIZE + size);

    new_block->size = block->size - size - BLOCK_HEADER_SIZE;
    new_block->free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
}

static void coalesce(void) {
    block_header_t* current = heap_head;
    while (current && current->next) {
        if (current->free && current->next->free) {
            current->size += BLOCK_HEADER_SIZE + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

static void kmem_try_reclaim_tail_pages(void) {
    block_header_t* last = find_last_block();
    if (!last || !last->free) {
        return;
    }

    u32 block_start = (u32)last;
    u32 block_end = block_start + BLOCK_HEADER_SIZE + last->size;
    u32 heap_end = (u32)(heap_start_ptr + heap_committed);

    if (block_end != heap_end) {
        return;
    }

    /*
     * Reclaim only full pages at the end of this free tail block.
     * Keep at least one page committed for the heap.
     */
    u32 reclaim_start = align_up(block_start + BLOCK_HEADER_SIZE, KMEM_PAGE_SIZE);
    u32 reclaim_end   = align_down(block_end, KMEM_PAGE_SIZE);

    if (reclaim_end <= reclaim_start) {
        return;
    }

    u32 reclaim_bytes = reclaim_end - reclaim_start;

    if (heap_committed <= KMEM_MIN_COMMITTED) {
        return;
    }

    if (reclaim_bytes > heap_committed - KMEM_MIN_COMMITTED) {
        reclaim_bytes = heap_committed - KMEM_MIN_COMMITTED;
        reclaim_bytes = align_down(reclaim_bytes, KMEM_PAGE_SIZE);
    }

    if (reclaim_bytes == 0) {
        return;
    }

    /*
     * Only reclaim from the very end of the committed heap.
     */
    if (reclaim_end != heap_end) {
        return;
    }

    u32 actual_reclaim_start = heap_end - reclaim_bytes;

    for (u32 addr = actual_reclaim_start; addr < heap_end; addr += KMEM_PAGE_SIZE) {
        u32 phys = paging_get_physical(addr);
        if (phys) {
            paging_unmap_page(addr);
            pmm_free_frame(phys & 0xFFFFF000u);
        }
    }

    heap_committed -= reclaim_bytes;

    /*
     * Shrink or remove the trailing free block metadata.
     */
    if (actual_reclaim_start <= block_start) {
        if (last == heap_head) {
            heap_head = 0;
        } else {
            block_header_t* current = heap_head;
            while (current && current->next != last) {
                current = current->next;
            }
            if (current) {
                current->next = 0;
            }
        }
        return;
    }

    last->size = actual_reclaim_start - block_start - BLOCK_HEADER_SIZE;
}

void kmem_init(void* heap_start, u32 heap_size) {
    heap_start_ptr = (u8*)heap_start;
    heap_total_size = heap_size;
    heap_committed = 0;
    heap_head = 0;

    kmem_expand(KMEM_PAGE_SIZE);
}

void* kmalloc(u32 size) {
    if (size == 0 || heap_start_ptr == 0) {
        return 0;
    }

    size = align_up(size, KMEM_ALIGN);

    for (;;) {
        block_header_t* current = heap_head;
        while (current) {
            if (current->free && current->size >= size) {
                split_block(current, size);
                current->free = 0;
                return (void*)((u8*)current + BLOCK_HEADER_SIZE);
            }
            current = current->next;
        }

        if (!kmem_expand(size + BLOCK_HEADER_SIZE)) {
            return 0;
        }
    }
}

void kfree(void* ptr) {
    if (!ptr) {
        return;
    }

    block_header_t* block =
        (block_header_t*)((u8*)ptr - BLOCK_HEADER_SIZE);

    // Validate that the pointer is within the heap
    if ((u8*)block < heap_start_ptr || (u8*)block >= heap_start_ptr + heap_committed) {
        return;
    }

    // Additional safety: check if block size is reasonable
    if (block->size == 0 || block->size > heap_total_size) {
        return;
    }

    block->free = 1;
    coalesce();
    kmem_try_reclaim_tail_pages();
}

u32 kmem_committed_size(void) {
    return heap_committed;
}

u32 kmem_max_size(void) {
    return heap_total_size;
}