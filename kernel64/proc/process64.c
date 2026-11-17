#include "process64.h"

#include "../arch/x86_64/context_switch64.h"
#include "../arch/x86_64/timer64.h"
#include "../mm/kmem64.h"
#include "../mm/paging64.h"
#include "../k64_console.h"
#include "../arch/x86_64/gdt64.h"
#include "../console/terminal64.h"
#include "../debug/bootlog64.h"
#include "../input/kinput64.h"
#include "../graphics/framebuffer64.h"
#include "../gui/gui64.h"

static process64_task_t g_tasks[PROCESS64_MAX_TASKS];

/*
 * Clean FXSAVE image copied into every new task.
 */
static nq_u8 g_fpu_template[PROCESS64_FPU_STATE_SIZE]
    __attribute__((aligned(16)));
static process64_task_t* g_current = 0;
static nq_u32 g_next_pid = 1;

static int g_preemption_enabled = 1;
static nq_u32 g_preempt_disable_depth = 0;
static nq_u64 g_scheduler_ticks = 0;
static nq_u64 g_preempt_switches = 0;
static nq_u64 g_preempt_blocked_ticks = 0;
static nq_u64 g_preempt_consecutive_blocked_ticks = 0;

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

static void mem_copy64(void* dst, const void* src, nq_u64 size) {
    nq_u8* d = (nq_u8*)dst;
    const nq_u8* s = (const nq_u8*)src;

    if (!d || !s) {
        return;
    }

    for (nq_u64 i = 0; i < size; i++) {
        d[i] = s[i];
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

/*
 * Enables x87/SSE and captures a clean register image.
 *
 * The kernel itself is built with -mgeneral-regs-only and never touches
 * these registers, so saving/restoring them at task switch is enough to
 * give every user program its own copy.
 */
static void process64_fpu_init(void) {
    nq_u64 cr0;
    nq_u64 cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ull << 2);  /* EM: no x87 emulation */
    cr0 &= ~(1ull << 3);  /* TS: no lazy-switch trap */
    cr0 |=  (1ull << 1);  /* MP */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9);   /* OSFXSR: FXSAVE/FXRSTOR and SSE enabled */
    cr4 |= (1ull << 10);  /* OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile ("fninit");
    __asm__ volatile ("fxsave64 (%0)" : : "r"(g_fpu_template) : "memory");

    /*
     * MXCSR default: all SSE exceptions masked, round to nearest.
     */
    g_fpu_template[24] = 0x80;
    g_fpu_template[25] = 0x1F;
    g_fpu_template[26] = 0x00;
    g_fpu_template[27] = 0x00;
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

        case PROCESS64_SLEEPING:
            return "SLEEP";

        default:
            return "UNKNOWN";
    }
}

static nq_u64 align_down64(nq_u64 value, nq_u64 align) {
    return value & ~(align - 1u);
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

static terminal64_session_t* process64_terminal_for_new_task(nq_u32 flags) {
    terminal64_session_t* term = 0;

    /*
     * spawn:
     *   private terminal, visible through view <pid>
     *
     * run / foreground:
     *   share the current terminal, so output appears directly in KER64.
     */
    if (flags & PROCESS64_SPAWN_PRIVATE_TERMINAL) {
        term = terminal64_create();

        if (term) {
            return term;
        }

        /*
         * If private terminal allocation fails, fall back safely to the
         * current terminal instead of failing task creation.
         */
    }

    term = process64_current_terminal();

    if (!term) {
        term = terminal64_kernel();
    }

    return term;
}

static void process64_user_memory_free(process64_task_t* task) {
    if (!task) {
        return;
    }

    if (task->user.image_backup) {
        kfree64(task->user.image_backup);
        task->user.image_backup = 0;
    }

    if (task->user.stack_backup) {
        kfree64(task->user.stack_backup);
        task->user.stack_backup = 0;
    }

    task->user.active = 0;
    task->user.image_base = 0;
    task->user.image_size = 0;
    task->user.stack_base = 0;
    task->user.stack_size = 0;
}

static void process64_user_memory_save_task(process64_task_t* task) {
    (void)task;

    /*
     * Old fixed-address user memory swapping has been replaced by
     * per-process CR3/PML4 address spaces.
     */
}


static void process64_user_memory_restore_task(process64_task_t* task) {
    (void)task;

    /*
     * Old fixed-address user memory swapping has been replaced by
     * per-process CR3/PML4 address spaces.
     */
}


int process64_user_memory_setup(
    nq_u64 image_base,
    nq_u64 image_size,
    nq_u64 stack_base,
    nq_u64 stack_size
) {
    process64_task_t* task = g_current;

    if (!task || task == &g_tasks[0]) {
        return 0;
    }

    if (image_base == 0 || image_size == 0 ||
        stack_base == 0 || stack_size == 0) {
        return 0;
    }

    /*
     * Real per-process address spaces are now responsible for isolating
     * user memory. Keep this metadata for ps/debug output, but do not
     * allocate image/stack backup buffers anymore.
     */
    process64_user_memory_free(task);

    task->user.image_base = image_base;
    task->user.image_size = image_size;
    task->user.stack_base = stack_base;
    task->user.stack_size = stack_size;
    task->user.image_backup = 0;
    task->user.stack_backup = 0;
    task->user.active = 1;

    return 1;
}


void process64_user_memory_save_current(void) {
    process64_user_memory_save_task(g_current);
}

void process64_user_memory_restore_current(void) {
    process64_user_memory_restore_task(g_current);
}

static nq_u64 process64_irq_save_disable(void) {
    nq_u64 flags;

    __asm__ volatile (
        "pushfq; pop %0; cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void process64_irq_restore(nq_u64 flags) {
    if (flags & 0x200ull) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

static void process64_task_trampoline(void) {
    process64_task_t* task = g_current;

    /*
     * Every context switch happens with interrupts disabled (timer IRQ or
     * process64_yield()), so a brand-new task always starts with IF=0.
     * Turn interrupts on, otherwise a task that never enters ring 3 would
     * run with the timer and keyboard dead.
     */
    __asm__ volatile ("sti");

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

    process64_fpu_init();
    mem_copy64(g_tasks[0].fpu_state, g_fpu_template, sizeof(g_fpu_template));

    g_tasks[0].pid = 0;
    g_tasks[0].state = PROCESS64_RUNNING;
    g_tasks[0].rsp = 0;
    g_tasks[0].stack_base = 0;
    g_tasks[0].stack_size = 0;
    g_tasks[0].pml4_phys = paging64_kernel_pml4();
    g_tasks[0].owns_address_space = 0;
    g_tasks[0].entry = 0;
    g_tasks[0].arg = 0;

    str_copy64(g_tasks[0].name, "kernel", sizeof(g_tasks[0].name));

    g_tasks[0].terminal = terminal64_kernel();
    g_tasks[0].terminal_id = terminal64_id(g_tasks[0].terminal);
    g_tasks[0].foreground = 1;

    g_current = &g_tasks[0];
    g_next_pid = 1;

    /*
     * Preemption is ON by default.
     */
    g_preemption_enabled = 1;
    g_preempt_disable_depth = 0;
    g_scheduler_ticks = 0;
    g_preempt_switches = 0;
    g_preempt_blocked_ticks = 0;
    g_preempt_consecutive_blocked_ticks = 0;
    g_quantum_ticks = PROCESS64_DEFAULT_QUANTUM_TICKS;

    bootlog64_print("Process64 initialized.\n");
}


int process64_current_create_user_address_space(void) {
    process64_task_t* task = g_current;

    if (!task || task == &g_tasks[0]) {
        return 0;
    }

    if (task->owns_address_space &&
        task->pml4_phys &&
        task->pml4_phys != paging64_kernel_pml4()) {
        paging64_switch_pml4(task->pml4_phys);
        return 1;
    }

    nq_u64 pml4_phys = paging64_create_user_address_space();

    if (!pml4_phys) {
        k64_print("process64: failed to create user address space\n");
        return 0;
    }

    task->pml4_phys = pml4_phys;
    task->owns_address_space = 1;

    paging64_switch_pml4(task->pml4_phys);

    return 1;
}

nq_u64 process64_current_pml4(void) {
    process64_task_t* task = g_current;

    if (!task || !task->pml4_phys) {
        return paging64_kernel_pml4();
    }

    return task->pml4_phys;
}

static int process64_create_kernel_task_locked(
    const char* name,
    process64_entry_t entry,
    void* arg,
    nq_u32 flags
);

int process64_create_kernel_task(
    const char* name,
    process64_entry_t entry,
    void* arg
) {
    /*
     * Backward-compatible default:
     * old callers behave like foreground/current-terminal tasks.
     */
    return process64_create_kernel_task_ex(
        name,
        entry,
        arg,
        PROCESS64_SPAWN_FOREGROUND
    );
}

int process64_create_kernel_task_ex(
    const char* name,
    process64_entry_t entry,
    void* arg,
    nq_u32 flags
) {
    if (!entry) {
        return -1;
    }

    /*
     * Task table, heap and terminal allocation are shared state. Keep the
     * timer from switching tasks until the new task is fully built.
     */
    process64_preempt_disable();

    int pid = process64_create_kernel_task_locked(name, entry, arg, flags);

    process64_preempt_enable();

    return pid;
}

static int process64_create_kernel_task_locked(
    const char* name,
    process64_entry_t entry,
    void* arg,
    nq_u32 flags
) {
    process64_task_t* task = 0;

    process64_reap_zombies();

    for (nq_u32 i = 1; i < PROCESS64_MAX_TASKS; i++) {
        if (g_tasks[i].state == PROCESS64_UNUSED) {
            task = &g_tasks[i];
            break;
        }
    }

    if (!task) {
        return -1;
    }

    void* stack = kmalloc64(PROCESS64_STACK_SIZE);

    if (!stack) {
        k64_print("process64: failed to allocate task stack. size=");
        k64_print_dec64(PROCESS64_STACK_SIZE);
        k64_print("\n");
        return -1;
    }

    mem_zero64(stack, PROCESS64_STACK_SIZE);

    mem_zero64(task, sizeof(*task));

    mem_copy64(task->fpu_state, g_fpu_template, sizeof(g_fpu_template));

    task->created_tick = timer64_get_ticks();
    task->pid = g_next_pid++;
    task->state = PROCESS64_READY;
    task->stack_base = (nq_u64)stack;
    task->stack_size = PROCESS64_STACK_SIZE;
    task->pml4_phys = paging64_kernel_pml4();
    task->owns_address_space = 0;
    task->entry = entry;
    task->arg = arg;

    if (flags & PROCESS64_SPAWN_PRIVATE_TERMINAL) {
        task->terminal = terminal64_create();

        if (task->terminal) {
            task->owns_terminal = 1;
        } else {
            task->terminal = process64_current_terminal();

            if (!task->terminal) {
                task->terminal = terminal64_kernel();
            }

            task->owns_terminal = 0;
        }
    } else {
        task->terminal = process64_current_terminal();

        if (!task->terminal) {
            task->terminal = terminal64_kernel();
        }

        task->owns_terminal = 0;
    }

    task->terminal_id = terminal64_id(task->terminal);
    task->foreground = (flags & PROCESS64_SPAWN_FOREGROUND) ? 1 : 0;

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

    if (!next->pml4_phys) {
        next->pml4_phys = paging64_kernel_pml4();
    }

    /*
     * Switch to the incoming task's address space before switching stacks.
     * Kernel stacks live in the direct map / shared kernel mappings, so they
     * remain valid across CR3 switches.
     */
    paging64_switch_pml4(next->pml4_phys);

    /*
     * If this task later enters ring 3, interrupts/syscalls from CPL3
     * will use TSS.rsp0. That must point to this task's kernel stack,
     * not the original boot stack.
     */
    if (next->stack_base && next->stack_size) {
        gdt64_set_rsp0(next->stack_base + next->stack_size);
    }

    /*
     * Swap x87/SSE register state along with the stack.
     */
    __asm__ volatile ("fxsave64 (%0)" : : "r"(prev->fpu_state) : "memory");
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(next->fpu_state) : "memory");

    context_switch64(&prev->rsp, next->rsp);
}


void process64_yield(void) {
    /*
     * Do not voluntarily switch while a kernel critical section is active.
     * Syscalls that intentionally yield/exit are not wrapped in the syscall
     * preemption guard, so normal SYS_YIELD still works.
     */
    if (process64_preempt_disabled()) {
        return;
    }

    /*
     * The switch itself must not be interrupted. process64_switch_to()
     * updates g_current before it changes stacks; a timer IRQ landing in
     * between would preempt "the new task" while still running on the old
     * task's stack and save the wrong RSP into it, destroying that task's
     * saved context.
     *
     * A task always resumes here with IF=0 (it is switched back in from
     * the timer IRQ or from another task's yield), then restores the
     * interrupt state it had when it called yield.
     */
    nq_u64 flags = process64_irq_save_disable();

    process64_task_t* next = process64_pick_next();

    if (next) {
        process64_switch_to(next);
    }

    process64_irq_restore(flags);
}


void process64_exit(void) {
    if (!g_current) {
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    /*
     * A dying task must not leave global preemption disabled.
     */
    while (g_preempt_disable_depth > 0) {
        g_preempt_disable_depth--;
    }

    g_current->state = PROCESS64_ZOMBIE;

    process64_yield();

    /*
     * If there was nowhere to yield, wait here as a zombie. Interrupts must
     * be on (exit can be reached from an exception handler with IF=0) so
     * the timer can switch away as soon as another task is ready.
     */
    for (;;) {
        __asm__ volatile ("sti");
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

void process64_preempt_disable(void) {
    /*
     * Single-CPU kernel preemption guard.
     *
     * Timer IRQs may still fire, but process64_on_timer_tick() must not
     * context-switch while this depth is nonzero.
     */
    g_preempt_disable_depth++;
}


void process64_preempt_enable(void) {
    if (g_preempt_disable_depth > 0) {
        g_preempt_disable_depth--;
    }
}


int process64_preempt_disabled(void) {
    return g_preempt_disable_depth != 0;
}

nq_u32 process64_preempt_depth(void) {
    return g_preempt_disable_depth;
}

void process64_on_timer_tick(void) {
    if (!g_preemption_enabled) {
        return;
    }

    if (!g_current) {
        return;
    }

    /*
     * Wake sleepers whose time has come. This only flips a state flag, so
     * it is safe even while a critical section holds the preemption guard.
     */
    int woke_task = 0;
    nq_u64 now = timer64_get_ticks();

    for (nq_u32 i = 0; i < PROCESS64_MAX_TASKS; i++) {
        if (g_tasks[i].state == PROCESS64_SLEEPING &&
            now >= g_tasks[i].wake_tick) {
            g_tasks[i].state = PROCESS64_READY;
            woke_task = 1;
        }
    }

    /*
     * Critical stability rule:
     *
     * Do not switch tasks while kernel/syscall/GUI code is inside a guarded
     * critical section. This was the root cause of app/window corruption.
     *
     * Do not print from here and do not force guard_depth to zero from here.
     * Printing from a timer IRQ can re-enter console/GUI code, and forcing the
     * guard open causes underflow when the guarded path returns normally.
     */
    if (process64_preempt_disabled()) {
        g_preempt_blocked_ticks++;
        g_preempt_consecutive_blocked_ticks++;
        return;
    }

    g_preempt_consecutive_blocked_ticks = 0;

    /*
     * Do not try to preempt if there is no other READY task.
     */
    process64_task_t* next = process64_pick_next();

    if (!next) {
        return;
    }

    g_scheduler_ticks++;

    /*
     * A task that just finished sleeping runs right away instead of waiting
     * for the current task's quantum to expire. That keeps sleep-paced apps
     * (frame timers) on time; a task that sleeps gives up the CPU, so this
     * cannot starve the others.
     */
    if (!woke_task && g_scheduler_ticks < g_quantum_ticks) {
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
    k64_print(" guard_depth=");
    k64_print_dec64(g_preempt_disable_depth);
    k64_print(" blocked_ticks=");
    k64_print_dec64(g_preempt_blocked_ticks);
    k64_print(" consecutive_blocked=");
    k64_print_dec64(g_preempt_consecutive_blocked_ticks);
    k64_print("\n");

    k64_print("  max_tasks=");
    k64_print_dec64(PROCESS64_MAX_TASKS);
    k64_print(" stack_size=");
    k64_print_dec64(PROCESS64_STACK_SIZE);
    k64_print(" input_queue=");
    k64_print_dec64(PROCESS64_INPUT_QUEUE_SIZE);
    k64_print("\n");

    k64_print("PID   STATE     TERM   FG   USER   AS    NAME\n");

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

        k64_print(g_tasks[i].user.active ? "Y" : "N");
        k64_print("      ");

        k64_print(g_tasks[i].owns_address_space ? "USR" : "KER");
        k64_print("   ");

        k64_print(g_tasks[i].name);
        k64_print("\n");
    }
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

nq_u32 process64_current_pid(void) {
    if (!g_current) {
        return 0;
    }

    return g_current->pid;
}

void process64_sleep_ticks(nq_u64 ticks) {
    nq_u64 start;

    if (ticks == 0) {
        return;
    }

    start = timer64_get_ticks();

    /*
     * Real sleep: mark this task SLEEPING and switch to another task. The
     * timer tick makes it READY again at wake_tick and schedules it. While
     * it sleeps it uses no CPU time, so other apps keep their frame rate.
     */
    if (g_current && !process64_preempt_disabled()) {
        nq_u64 flags = process64_irq_save_disable();
        process64_task_t* next = process64_pick_next();

        if (next) {
            g_current->wake_tick = start + ticks;
            g_current->state = PROCESS64_SLEEPING;

            process64_switch_to(next);

            process64_irq_restore(flags);
            return;
        }

        process64_irq_restore(flags);
    }

    /*
     * Nothing else to run (or we are inside a guarded section): wait for
     * timer interrupts in place.
     */
    while ((timer64_get_ticks() - start) < ticks) {
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
        __asm__ volatile ("cli");
    }
}

int process64_is_alive(nq_u32 pid) {
    process64_task_t* task = process64_find_by_pid(pid);

    if (!task) {
        return 0;
    }

    if (task->state == PROCESS64_UNUSED ||
        task->state == PROCESS64_ZOMBIE) {
        return 0;
    }

    return 1;
}

void process64_wait_pid(nq_u32 pid) {
    while (process64_is_alive(pid)) {
        process64_yield();
    }
}

static process64_task_t* process64_foreground_task(void) {
    for (nq_u32 i = 0; i < PROCESS64_MAX_TASKS; i++) {
        process64_task_t* task = &g_tasks[i];

        if (task->state == PROCESS64_UNUSED ||
            task->state == PROCESS64_ZOMBIE) {
            continue;
        }

        if (task->foreground) {
            return task;
        }
    }

    return &g_tasks[0];
}

int process64_has_foreground_app(void) {
    process64_task_t* task = process64_foreground_task();

    if (!task) {
        return 0;
    }

    return task->pid != 0;
}

nq_u32 process64_foreground_pid(void) {
    process64_task_t* task = process64_foreground_task();

    if (!task) {
        return 0;
    }

    return task->pid;
}

static int process64_task_input_empty(process64_task_t* task) {
    if (!task) {
        return 1;
    }

    return task->input_head == task->input_tail;
}

static int process64_task_input_full(process64_task_t* task) {
    nq_u32 next;

    if (!task) {
        return 1;
    }

    next = (task->input_tail + 1u) % PROCESS64_INPUT_QUEUE_SIZE;

    return next == task->input_head;
}

static int process64_task_push_key(process64_task_t* task, char c) {
    nq_u32 next;

    if (!task || c == 0) {
        return 0;
    }

    next = (task->input_tail + 1u) % PROCESS64_INPUT_QUEUE_SIZE;

    if (next == task->input_head) {
        return 0;
    }

    task->input_queue[task->input_tail] = c;
    task->input_tail = next;

    return 1;
}

static int process64_task_pop_key(process64_task_t* task) {
    char c;

    if (!task || process64_task_input_empty(task)) {
        return 0;
    }

    c = task->input_queue[task->input_head];
    task->input_head =
        (task->input_head + 1u) % PROCESS64_INPUT_QUEUE_SIZE;

    return (int)(nq_u8)c;
}

int process64_push_key_to_foreground(char c) {
    process64_task_t* task = process64_foreground_task();

    if (!task || task->pid == 0) {
        return 0;
    }

    return process64_task_push_key(task, c);
}

int process64_push_key_to_pid(nq_u32 pid, char c) {
    process64_task_t* task = process64_find_by_pid(pid);

    if (!task || task->pid == 0 ||
        task->state == PROCESS64_ZOMBIE) {
        return 0;
    }

    return process64_task_push_key(task, c);
}

static void process64_fill_info(process64_task_t* task, process64_info_t* out) {
    out->pid = task->pid;
    out->name = task->name;
    out->terminal = task->terminal;
    out->owns_terminal = task->owns_terminal;
    out->created_tick = task->created_tick;
}

int process64_task_info(nq_u32 slot, process64_info_t* out) {
    if (slot >= PROCESS64_MAX_TASKS || !out) {
        return 0;
    }

    process64_task_t* task = &g_tasks[slot];

    if (task->state == PROCESS64_UNUSED ||
        task->state == PROCESS64_ZOMBIE) {
        return 0;
    }

    process64_fill_info(task, out);

    return 1;
}

int process64_info_for_pid(nq_u32 pid, process64_info_t* out) {
    process64_task_t* task = process64_find_by_pid(pid);

    if (!task || !out || task->state == PROCESS64_ZOMBIE) {
        return 0;
    }

    process64_fill_info(task, out);

    return 1;
}

int process64_input_poll_key(void) {
    int c;

    /*
     * First check this process's private input queue.
     */
    c = process64_task_pop_key(g_current);

    if (c != 0) {
        return c;
    }

    /*
     * POC direct foreground input path:
     *
     * If the currently running task is a foreground user/app task,
     * it may pull directly from the global keyboard queue.
     *
     * This fixes run <app>, where the shell is waiting and may not
     * be actively routing keyboard input into the app queue.
     */
    if (g_current && g_current->pid != 0 && g_current->foreground) {
        return kinput64_poll_char();
    }

    return 0;
}

int process64_input_get_key(void) {
    int c;

    for (;;) {
        c = process64_input_poll_key();

        if (c != 0) {
            return c;
        }

        /*
         * Do NOT process64_yield() from inside a syscall loop.
         * That can break the syscall/iret return path.
         *
         * Wait for an interrupt, then check the queue again.
         */
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
        __asm__ volatile ("cli");
    }
}

static void process64_reap_task(process64_task_t* task) {
    if (!task) {
        return;
    }

    if (task == &g_tasks[0]) {
        return;
    }

    if (task->state != PROCESS64_ZOMBIE) {
        return;
    }

    /*
     * Free saved user image/stack backups for fixed-address app swapping.
     */
    process64_user_memory_free(task);

    if (task->owns_address_space &&
        task->pml4_phys &&
        task->pml4_phys != paging64_kernel_pml4()) {
        paging64_destroy_user_address_space(task->pml4_phys);
        task->pml4_phys = paging64_kernel_pml4();
        task->owns_address_space = 0;
    }

    /*
     * Free kernel task stack.
     */
    if (task->stack_base) {
        kfree64((void*)task->stack_base);
        task->stack_base = 0;
        task->stack_size = 0;
        task->rsp = 0;
    }

    /*
     * Release the private terminal session so it can be reused; there are
     * only TERM64_MAX_SESSIONS of them. terminal64_destroy() switches the
     * text console back to the kernel terminal if this one was showing.
     */
    if (task->owns_terminal && task->terminal) {
        /*
         * If a desktop window is showing this terminal, the window keeps it
         * (so the output stays readable after the app exits) and frees it
         * when the window is closed.
         */
        if (!gui64_adopt_terminal(task->terminal, (int)task->pid, task->name)) {
            terminal64_destroy(task->terminal);
        }
    }

    if (task->gfx.buffer) {
        kfree64(task->gfx.buffer);
        task->gfx.buffer = 0;
    }

    /*
     * Files the app left open.
     */
    for (nq_u32 i = 0; i < PROCESS64_MAX_FILES; i++) {
        if (task->files[i].data) {
            kfree64(task->files[i].data);
            task->files[i].data = 0;
        }

        task->files[i].used = 0;
    }

    task->gfx.mode = PROCESS64_GFX_NONE;
    task->gfx.width = 0;
    task->gfx.height = 0;
    task->gfx.buffer_size = 0;
    task->gfx.dirty = 0;

    task->terminal = 0;
    task->terminal_id = 0;
    task->owns_terminal = 0;
    task->foreground = 0;

    task->entry = 0;
    task->arg = 0;

    /*
     * Clear input queue if present.
     */
#ifdef PROCESS64_INPUT_QUEUE_SIZE
    task->input_head = 0;
    task->input_tail = 0;
#endif

    if (task->pid != 0) {
        gui64_destroy_windows_for_pid((int)task->pid);
    }

    task->name[0] = 0;
    task->pid = 0;
    task->state = PROCESS64_UNUSED;
}

void process64_reap_zombies(void) {
    /*
     * Reaping frees page tables, physical frames, heap blocks and windows.
     * Do not let the timer switch to an app in the middle of that.
     */
    process64_preempt_disable();

    for (nq_u32 i = 1; i < PROCESS64_MAX_TASKS; i++) {
        /*
         * Never free the stack we are currently running on.
         */
        if (&g_tasks[i] == g_current) {
            continue;
        }

        if (g_tasks[i].state == PROCESS64_ZOMBIE) {
            process64_reap_task(&g_tasks[i]);
        }
    }

    process64_preempt_enable();
}

int process64_reap_pid(nq_u32 pid) {
    process64_task_t* task = process64_find_by_pid(pid);

    if (!task) {
        return 0;
    }

    if (task == &g_tasks[0]) {
        return 0;
    }

    if (task->state != PROCESS64_ZOMBIE || task == g_current) {
        return 0;
    }

    process64_preempt_disable();
    process64_reap_task(task);
    process64_preempt_enable();

    return 1;
}

nq_u32 process64_zombie_count(void) {
    nq_u32 count = 0;

    for (nq_u32 i = 1; i < PROCESS64_MAX_TASKS; i++) {
        if (g_tasks[i].state == PROCESS64_ZOMBIE) {
            count++;
        }
    }

    return count;
}

static nq_u64 str_len64(const char* s) {
    nq_u64 n = 0;

    if (!s) {
        return 0;
    }

    while (s[n]) {
        n++;
    }

    return n;
}

static void str_copy_bounded64(char* dst, const char* src, nq_u64 dst_size) {
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

int process64_file_open(const char* path, nq_u8* data, nq_u64 size) {
    process64_task_t* task = process64_current();

    if (!task || !path || !path[0]) {
        return -1;
    }

    for (nq_u32 i = 0; i < PROCESS64_MAX_FILES; i++) {
        if (!task->files[i].used) {
            task->files[i].used = 1;
            task->files[i].data = data;
            task->files[i].size = size;
            task->files[i].offset = 0;
            str_copy_bounded64(
                task->files[i].path,
                path,
                sizeof(task->files[i].path)
            );

            /*
             * Return handles starting at 3 so 0/1/2 can later be stdin/out/err.
             */
            return (int)(i + 3);
        }
    }

    return -1;
}

int process64_file_close(int handle) {
    process64_task_t* task = process64_current();

    if (!task) {
        return -1;
    }

    if (handle < 3) {
        return -1;
    }

    nq_u32 idx = (nq_u32)(handle - 3);

    if (idx >= PROCESS64_MAX_FILES) {
        return -1;
    }

    if (!task->files[idx].used) {
        return -1;
    }

    if (task->files[idx].data) {
        kfree64(task->files[idx].data);
        task->files[idx].data = 0;
    }

    task->files[idx].used = 0;
    task->files[idx].path[0] = 0;
    task->files[idx].size = 0;
    task->files[idx].offset = 0;

    return 0;
}

process64_file_t* process64_file_from_handle(int handle) {
    process64_task_t* task = process64_current();

    if (!task) {
        return 0;
    }

    if (handle < 3) {
        return 0;
    }

    nq_u32 idx = (nq_u32)(handle - 3);

    if (idx >= PROCESS64_MAX_FILES) {
        return 0;
    }

    if (!task->files[idx].used) {
        return 0;
    }

    return &task->files[idx];
}

int process64_set_graphics_mode(nq_u32 mode) {
    process64_task_t* task = process64_current();

    if (!task || task->pid == 0) {
        return 0;
    }

    if (mode == 0) {
        task->gfx.mode = PROCESS64_GFX_NONE;
        return 1;
    }

    if (!task->gfx.buffer) {
        task->gfx.width = PROCESS64_GFX_MAX_WIDTH;
        task->gfx.height = PROCESS64_GFX_MAX_HEIGHT;
        task->gfx.buffer_size =
            (nq_u64)task->gfx.width *
            (nq_u64)task->gfx.height *
            sizeof(nq_u32);

        task->gfx.buffer = (nq_u32*)kmalloc64(task->gfx.buffer_size);

        if (!task->gfx.buffer) {
            task->gfx.width = 0;
            task->gfx.height = 0;
            task->gfx.buffer_size = 0;
            return 0;
        }
    }

    task->gfx.mode = PROCESS64_GFX_ACTIVE;
    task->gfx.dirty = 1;

    /*
    * If a foreground app switches into graphics mode, it now owns the
    * visible framebuffer. Clear old KER64 terminal pixels immediately.
    */
    if (task->foreground) {
        fb64_clear(fb64_make_color(8, 16, 28));
    }

    return 1;
}

int process64_present_current_gfx(
    const nq_u32* pixels,
    nq_u32 width,
    nq_u32 height,
    int dst_x,
    int dst_y
) {
    process64_task_t* task = process64_current();

    if (!task || !pixels) {
        return 0;
    }

    if (task->gfx.mode != PROCESS64_GFX_ACTIVE) {
        return 0;
    }

    if (!task->gfx.buffer) {
        return 0;
    }

    if (width == 0 || height == 0) {
        return 0;
    }

    if (width > task->gfx.width || height > task->gfx.height) {
        return 0;
    }

    for (nq_u32 y = 0; y < height; y++) {
        for (nq_u32 x = 0; x < width; x++) {
            task->gfx.buffer[y * task->gfx.width + x] =
                pixels[y * width + x];
        }
    }

    task->gfx.frame_width = width;
    task->gfx.frame_height = height;
    task->gfx.dst_x = dst_x;
    task->gfx.dst_y = dst_y;
    task->gfx.dirty = 1;

    return 1;
}

int process64_show_graphics(nq_u32 pid) {
    process64_task_t* task = process64_find_by_pid(pid);

    if (!task || !task->gfx.buffer ||
        task->gfx.mode != PROCESS64_GFX_ACTIVE) {
        return 0;
    }

    fb64_clear(fb64_make_color(8, 16, 28));

    fb64_blit_rgb32(
        task->gfx.dst_x,
        task->gfx.dst_y,
        task->gfx.frame_width ? task->gfx.frame_width : task->gfx.width,
        task->gfx.frame_height ? task->gfx.frame_height : task->gfx.height,
        task->gfx.buffer
    );

    return 1;
}

nq_u32 process64_get_graphics_mode(void) {
    process64_task_t* task = process64_current();

    if (!task) {
        return PROCESS64_GFX_NONE;
    }

    return (nq_u32)task->gfx.mode;
}

int process64_task_has_graphics(nq_u32 pid) {
    process64_task_t* task = process64_find_by_pid(pid);

    if (!task) {
        return 0;
    }

    if (task->state == PROCESS64_UNUSED) {
        return 0;
    }

    if (task->gfx.mode != PROCESS64_GFX_ACTIVE) {
        return 0;
    }

    if (!task->gfx.buffer) {
        return 0;
    }

    return 1;
}

int process64_current_is_foreground(void) {
    return g_current && g_current->foreground;
}