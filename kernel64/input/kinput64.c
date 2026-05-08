#include "kinput64.h"

#define KINPUT64_QUEUE_SIZE 128

static volatile char g_key_queue[KINPUT64_QUEUE_SIZE];
static volatile nq_u32 g_key_head = 0;
static volatile nq_u32 g_key_tail = 0;

static nq_u64 irq_save_disable(void) {
    nq_u64 flags;

    __asm__ volatile (
        "pushfq\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void irq_restore(nq_u64 flags) {
    if (flags & 0x200ull) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

void kinput64_init(void) {
    nq_u64 flags = irq_save_disable();

    g_key_head = 0;
    g_key_tail = 0;

    irq_restore(flags);
}

void kinput64_push_char(char c) {
    if (c == 0) {
        return;
    }

    nq_u32 next_tail = (g_key_tail + 1u) % KINPUT64_QUEUE_SIZE;

    /*
     * Queue full: drop the key.
     */
    if (next_tail == g_key_head) {
        return;
    }

    g_key_queue[g_key_tail] = c;
    g_key_tail = next_tail;
}

int kinput64_poll_char(void) {
    nq_u64 flags = irq_save_disable();

    if (g_key_head == g_key_tail) {
        irq_restore(flags);
        return 0;
    }

    char c = g_key_queue[g_key_head];
    g_key_head = (g_key_head + 1u) % KINPUT64_QUEUE_SIZE;

    irq_restore(flags);

    return (int)c;
}

char kinput64_read_char(void) {
    for (;;) {
        int c = kinput64_poll_char();

        if (c != 0) {
            return (char)c;
        }

        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
}