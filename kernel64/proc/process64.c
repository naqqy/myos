#include "process64.h"

#include "../arch/x86_64/context_switch64.h"
#include "../mm/kmem64.h"
#include "../k64_console.h"
#include "../arch/x86_64/gdt64.h"
#include "../console/terminal64.h"
#include "../debug/bootlog64.h"

static process64_task_t g_tasks[PROCESS64_MAX_TASKS];
static process64_task_t* g_current = 0;
static nq_u32 g_next_pid = 1;

static int g_preemption_enabled = 0;
static nq_u64 g_scheduler_ticks = 0;
static nq_u64 g_preempt_switches = 0;

/*
 * Round-robin quantum in timer ticks.
 * At 100 Hz, 5 ticks = about 50ms.
 */
#define PROCESS64_DEFAULT_QUANTUM_TICKS 5u
static nq_u32 g_quantum_ticks = PROCESS64_DEFAULT_QUANTUM_TICKS;

static void mem_zero64(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    if (!p) {
        return;
    }

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void str_copy64(char* dst, const char* src, nq_u64 dst_size) {
    nq_u64 i = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = 0;
        return;
    }

    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static const char* process64_state_name(process64_state_t state) {
    switch (state) {
        case PROCESS64_UNUSED:
            return "UNUSED";

        case PROCESS64_READY:
            return "READY";

        case PROCESS64_RUNNING:
            return "RUNNING";

        case PROCESS64_ZOMBIE:
            return "ZOMBIE";

        default:
            return "UNKNOWN";
    }
}

static nq_u64 align_down64(nq_u64 value, nq_u64 align) {
    return value & ~(align - 1u);
}

static void process64_task_trampoline(void) {
    process64_task_t* task = g_current;

    if (!task || !task->entry) {
        process64_exit();
    }

    task->entry(task->arg);

    process64_exit();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static nq_u64 process64_build_initial_stack(nq_u64 stack_base, nq_u64 stack_size) {
    nq_u64 stack_top = stack_base + stack_size;

    /*
     * We want RSP to be 8 mod 16 after RET enters the trampoline,
     * matching the normal SysV AMD64 function-entry alignment.
     */
    nq_u64 sp = align_down64(stack_top, 16);

    /*
     * Leave 8 bytes of padding above the return address.
     */
    sp -= 8;

    /*
     * Return address for context_switch64's final RET.
     */
    sp -= 8;
    *((nq_u64*)sp) = (nq_u64)process64_task_trampoline;

    /*
     * Saved callee-saved registers expected by context_switch64:
     *
     * context_switch64 restores:
     *   pop r15
     *   pop r14
     *   pop r13
     *   pop r12
     *   pop rbp
     *   pop rbx
     *   ret
     */
    sp -= 8;
    *((nq_u64*)sp) = 0; /* rbx */

    sp -= 8;
    *((nq_u64*)sp) = 0; /* rbp */

    sp -= 8;
    *((nq_u64*)sp) = 0; /* r12 */

    sp -= 8;
    *((nq_u64*)sp) = 0; /* r13 */

    sp -= 8;
    *((nq_u64*)sp) = 0; /* r14 */

    sp -= 8;
    *((nq_u64*)sp) = 0; /* r15 */

    return sp;
}

void process64_init(void) {
    mem_zero64(g_tasks, sizeof(g_tasks));

    g_tasks[0].pid = 0;
    g_tasks[0].state = PROCESS64_RUNNING;
    g_tasks[0].rsp = 0;
    g_tasks[0].stack_base = 0;
    g_tasks[0].stack_size = 0;
    g_tasks[0].entry = 0;
    g_tasks[0].arg = 0;

    str_copy64(g_tasks[0].name, "kernel", sizeof(g_tasks[0].name));

    g_tasks[0].terminal = terminal64_kernel();
    g_tasks[0].terminal_id = terminal64_id(g_tasks[0].terminal);
    g_tasks[0].foreground = 1;

    g_current = &g_tasks[0];
    g_next_pid = 1;

    g_preemption_enabled = 0;
    g_scheduler_ticks = 0;
    g_preempt_switches = 0;
    g_quantum_ticks = PROCESS64_DEFAULT_QUANTUM_TICKS;

    bootlog64_print("Process64 initialized.\n");
}

int process64_create_kernel_task(
    const char* name,
    process64_entry_t entry,
    void* arg
) {
    if (!entry) {
        return -1;
    }

    process64_task_t* task = 0;

    for (nq_u32 i = 1; i < PROCESS64_MAX_TASKS; i++) {
        if (g_tasks[i].state == PROCESS64_UNUSED ||
            g_tasks[i].state == PROCESS64_ZOMBIE) {
            task = &g_tasks[i];
            break;
        }
    }

    if (!task) {
        return -1;
    }

    void* stack = kmalloc64(PROCESS64_STACK_SIZE);

    if (!stack) {
        return -1;
    }

    mem_zero64(stack, PROCESS64_STACK_SIZE);

    mem_zero64(task, sizeof(*task));

    task->pid = g_next_pid++;
    task->state = PROCESS64_READY;
    task->stack_base = (nq_u64)stack;
    task->stack_size = PROCESS64_STACK_SIZE;
    task->entry = entry;
    task->arg = arg;

    task->terminal = terminal64_create();

    if (!task->terminal) {
        task->terminal = terminal64_kernel();
    }

    task->terminal_id = terminal64_id(task->terminal);
    task->foreground = 0;

    str_copy64(task->name, name ? name : "task", sizeof(task->name));

    task->rsp = process64_build_initial_stack(
        task->stack_base,
        task->stack_size
    );

    return (int)task->pid;
}

static process64_task_t* process64_pick_next(void) {
    if (!g_current) {
        return 0;
    }

    nq_u32 start = 0;

    for (nq_u32 i = 0; i < PROCESS64_MAX_TASKS; i++) {
        if (&g_tasks[i] == g_current) {
            start = i;
            break;
        }
    }

    for (nq_u32 step = 1; step <= PROCESS64_MAX_TASKS; step++) {
        nq_u32 idx = (start + step) % PROCESS64_MAX_TASKS;

        if (g_tasks[idx].state == PROCESS64_READY) {
            return &g_tasks[idx];
        }
    }

    return 0;
}

static void process64_switch_to(process64_task_t* next) {
    process64_task_t* prev = g_current;

    if (!prev || !next || prev == next) {
        return;
    }

    if (prev->state == PROCESS64_RUNNING) {
        prev->state = PROCESS64_READY;
    }

    next->state = PROCESS64_RUNNING;
    g_current = next;

    /*
     * If this task later enters ring 3, interrupts/syscalls from CPL3
     * will use TSS.rsp0. That must point to this task's kernel stack,
     * not the original boot stack.
     */
    if (next->stack_base && next->stack_size) {
        gdt64_set_rsp0(next->stack_base + next->stack_size);
    }

    context_switch64(&prev->rsp, next->rsp);
}

void process64_yield(void) {
    process64_task_t* next = process64_pick_next();

    if (!next) {
        return;
    }

    process64_switch_to(next);
}

void process64_exit(void) {
    if (!g_current) {
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    g_current->state = PROCESS64_ZOMBIE;

    process64_yield();

    /*
     * If there was nowhere to yield, stop here.
     */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

int process64_kill(nq_u32 pid) {
    for (nq_u32 i = 0; i < PROCESS64_MAX_TASKS; i++) {
        process64_task_t* task = &g_tasks[i];

        if (task->state == PROCESS64_UNUSED) {
            continue;
        }

        if (task->pid != pid) {
            continue;
        }

        /*
         * Do not kill the base kernel/shell task.
         */
        if (task == &g_tasks[0]) {
            return 0;
        }

        /*
         * If killing the currently running task, exit cleanly.
         */
        if (task == g_current) {
            process64_exit();
            return 1;
        }

        /*
         * POC kill:
         * Mark as ZOMBIE. Later task reaping will free stack/memory.
         */
        task->state = PROCESS64_ZOMBIE;
        return 1;
    }

    return 0;
}

void process64_enable_preemption(void) {
    g_preemption_enabled = 1;
}

void process64_disable_preemption(void) {
    g_preemption_enabled = 0;
}

int process64_preemption_enabled(void) {
    return g_preemption_enabled;
}

void process64_on_timer_tick(void) {
    if (!g_preemption_enabled) {
        return;
    }

    if (!g_current) {
        return;
    }

    /*
     * Do not try to preempt if there is no other READY task.
     */
    process64_task_t* next = process64_pick_next();

    if (!next) {
        return;
    }

    g_scheduler_ticks++;

    if (g_scheduler_ticks < g_quantum_ticks) {
        return;
    }

    g_scheduler_ticks = 0;
    g_preempt_switches++;

    process64_switch_to(next);
}

void process64_schedule(void) {
    process64_yield();
}

process64_task_t* process64_current(void) {
    return g_current;
}

void process64_print_summary(void) {
    k64_print("Process64:\n");

    k64_print("  preemption=");
    k64_print(g_preemption_enabled ? "on" : "off");
    k64_print(" quantum_ticks=");
    k64_print_dec64(g_quantum_ticks);
    k64_print(" preempt_switches=");
    k64_print_dec64(g_preempt_switches);
    k64_print("\n");

    k64_print("PID   STATE     TERM   FG   NAME\n");

    for (nq_u32 i = 0; i < PROCESS64_MAX_TASKS; i++) {
        if (g_tasks[i].state == PROCESS64_UNUSED) {
            continue;
        }

        k64_print_dec64(g_tasks[i].pid);
        k64_print("     ");

        k64_print(process64_state_name(g_tasks[i].state));
        k64_print("   ");

        k64_print_dec64(g_tasks[i].terminal_id);
        k64_print("      ");

        k64_print(g_tasks[i].foreground ? "Y" : "N");
        k64_print("    ");

        k64_print(g_tasks[i].name);
        k64_print("\n");
    }
}

static process64_task_t* process64_find_by_pid(nq_u32 pid) {
    for (nq_u32 i = 0; i < PROCESS64_MAX_TASKS; i++) {
        if (g_tasks[i].state == PROCESS64_UNUSED) {
            continue;
        }

        if (g_tasks[i].pid == pid) {
            return &g_tasks[i];
        }
    }

    return 0;
}

terminal64_session_t* process64_terminal(process64_task_t* task) {
    if (!task || !task->terminal) {
        return terminal64_kernel();
    }

    return task->terminal;
}

terminal64_session_t* process64_current_terminal(void) {
    return process64_terminal(g_current);
}

void process64_write_current(const char* s) {
    terminal64_write(process64_current_terminal(), s);
}

void process64_write_current_len(const char* s, nq_u64 len) {
    terminal64_write_len(process64_current_terminal(), s, len);
}

int process64_view_terminal(nq_u32 pid) {
    process64_task_t* task;

    if (pid == 0) {
        terminal64_switch(terminal64_kernel());
        return 1;
    }

    task = process64_find_by_pid(pid);

    if (!task || !task->terminal) {
        return 0;
    }

    terminal64_switch(task->terminal);
    return 1;
}

int process64_foreground(nq_u32 pid) {
    process64_task_t* task;

    for (nq_u32 i = 0; i < PROCESS64_MAX_TASKS; i++) {
        g_tasks[i].foreground = 0;
    }

    if (pid == 0) {
        g_tasks[0].foreground = 1;
        terminal64_switch(terminal64_kernel());
        return 1;
    }

    task = process64_find_by_pid(pid);

    if (!task || !task->terminal) {
        return 0;
    }

    task->foreground = 1;
    terminal64_switch(task->terminal);
    return 1;
}