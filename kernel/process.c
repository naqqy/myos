#include "process.h"
#include "paging.h"
#include "kmem.h"
#include "pmm.h"
#include "gdt.h"
#include "usermode.h"

typedef unsigned int u32;
typedef unsigned char u8;

extern void kernel_print(const char* s);
extern volatile int kill_current_process_requested;
extern void context_switch(u32* old_esp, u32 new_esp);
extern volatile int schedule_requested;
extern volatile int foreground_escape_requested;

#define PROCESS_KERNEL_STACK_SIZE 16384
#define PROCESS_USER_STACK_TOP        0xC0000000u
#define PROCESS_USER_STACK_PAGE_COUNT 4
#define PROCESS_USER_STACK_SIZE       (PROCESS_USER_STACK_PAGE_COUNT * 4096)

static process_t process_table[PROCESS_MAX_COUNT];
static u32 next_pid = 1;
static process_t* current_process = 0;
static u32 kernel_context_esp = 0;
static u32 last_scheduled_index = 0;
static volatile u32 preempt_disable_count = 0;
static u32 foreground_pid = 0;

void preempt_disable(void) {
    preempt_disable_count++;
}

void preempt_enable(void) {
    if (preempt_disable_count > 0) {
        preempt_disable_count--;
    }
}

static u32 process_kernel_stack_top(process_t* process) {
    if (!process || !process->kernel_stack_base) {
        return 0;
    }

    return (u32)((u8*)process->kernel_stack_base + process->kernel_stack_size);
}

int process_foreground_escape_requested(void) {
    return foreground_escape_requested != 0;
}

void process_clear_foreground_escape_request(void) {
    foreground_escape_requested = 0;
}

int preempt_can_switch(void) {
    return preempt_disable_count == 0;
}

static void str_copy(char* dst, const char* src, u32 max_len) {
    u32 i = 0;

    if (max_len == 0) {
        return;
    }

    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static void mem_zero(void* ptr, u32 size) {
    u8* p = (u8*)ptr;

    for (u32 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static process_t* process_alloc_slot(void) {
    for (u32 i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (process_table[i].state == PROCESS_STATE_UNUSED) {
            return &process_table[i];
        }
    }

    return 0;
}

static process_t* process_select_next(void) {
    for (u32 step = 0; step < PROCESS_MAX_COUNT; step++) {
        u32 idx = (last_scheduled_index + step) % PROCESS_MAX_COUNT;
        process_t* p = &process_table[idx];

        if (p->state == PROCESS_STATE_READY) {
            last_scheduled_index = (idx + 1) % PROCESS_MAX_COUNT;
            return p;
        }
    }

    return 0;
}

static void process_prepare_initial_stack(process_t* process);

static void process_enter_user_mode(process_t* process) {
    if (!process) {
        return;
    }

    if (!process->user_mode_ready) {
        return;
    }

    if (!process->image.entry_addr) {
        return;
    }

    if (!process->user_stack_top) {
        return;
    }

    /*
     * This does not return if the user program calls SYS_EXIT.
     */
    usermode_enter(process->image.entry_addr, process->user_stack_top);

    /*
     * If usermode_enter ever returns, the caller treats that as failure.
     */
}

static void process_bootstrap_current(void) {
    process_t* process = current_process;

    if (!process) {
        for (;;) {
            __asm__ volatile ("cli");
            __asm__ volatile ("hlt");
        }
    }

    process->state = PROCESS_STATE_RUNNING;

    /*
     * All apps are now expected to be user-mode ELF programs.
     * They communicate with the kernel through int 0x80 syscalls.
     */
    if (process->run_in_user_mode &&
        process->user_mode_ready &&
        process->image.entry_addr &&
        process->user_stack_top) {
        process_enter_user_mode(process);
    }

    /*
     * Reaching here means entering user mode failed, or user code returned
     * without exiting through SYS_EXIT.
     */
    if (process_kill_requested()) {
        process->exit_code = -2;
    } else {
        process->exit_code = -1;
    }

    process->state = PROCESS_STATE_EXITED;
    process_clear_kill_request();
    current_process = 0;

    context_switch(&process->context.esp, kernel_context_esp);

    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

static void process_prepare_initial_stack(process_t* process) {
    u32* stack_top =
        (u32*)((u8*)process->kernel_stack_base + process->kernel_stack_size);

    /*
     * context_switch restores:
     *   pop edi
     *   pop esi
     *   pop ebx
     *   pop ebp
     *   ret
     */
    *(--stack_top) = (u32)process_bootstrap_current; /* ret */
    *(--stack_top) = 0; /* ebp */
    *(--stack_top) = 0; /* ebx */
    *(--stack_top) = 0; /* esi */
    *(--stack_top) = 0; /* edi */

    process->context.esp = (u32)stack_top;
    process->has_started = 1;
}

void process_table_init(void) {
    mem_zero(process_table, sizeof(process_table));
    next_pid = 1;
    current_process = 0;
    kernel_context_esp = 0;
    last_scheduled_index = 0;
    preempt_disable_count = 0;
    foreground_pid = 0;

    paging_switch_kernel_directory();
}

static int process_alloc_user_stack(process_t* process) {
    if (!process || !process->page_directory) {
        return 0;
    }

    if (PROCESS_USER_STACK_PAGE_COUNT > 8) {
        return 0;
    }

    process->user_stack_top = PROCESS_USER_STACK_TOP;
    process->user_stack_bottom = PROCESS_USER_STACK_TOP - PROCESS_USER_STACK_SIZE;
    process->user_stack_page_count = 0;

    for (u32 i = 0; i < PROCESS_USER_STACK_PAGE_COUNT; i++) {
        u32 virt = process->user_stack_bottom + i * 4096;
        u32 phys = pmm_alloc_frame();

        if (!phys) {
            kernel_print("process: user stack frame allocation failed\n");
            return 0;
        }

        /*
         * Early kernel still identity maps low physical memory.
         * This lets us clear the physical frame directly for now.
         */
        mem_zero((void*)phys, 4096);

        if (!paging_map_page_in_directory(
                process->page_directory,
                virt,
                phys,
                PAGE_WRITABLE | PAGE_USER)) {
            pmm_free_frame(phys);
            kernel_print("process: user stack page map failed\n");
            return 0;
        }

        process->user_stack_pages[process->user_stack_page_count++] = phys;
    }

    return 1;
}

static void process_free_user_stack(process_t* process) {
    if (!process || !process->page_directory) {
        return;
    }

    for (u32 i = 0; i < process->user_stack_page_count; i++) {
        u32 virt = process->user_stack_bottom + i * 4096;

        paging_unmap_page_in_directory(process->page_directory, virt);

        if (process->user_stack_pages[i]) {
            pmm_free_frame(process->user_stack_pages[i]);
            process->user_stack_pages[i] = 0;
        }
    }

    process->user_stack_top = 0;
    process->user_stack_bottom = 0;
    process->user_stack_page_count = 0;
}

process_t* process_spawn(const char* filename) {
    preempt_disable();

    process_t* process;

    if (!filename) {
        preempt_enable();
        return 0;
    }

    process = process_alloc_slot();
    if (!process) {
        kernel_print("process: no free process slots\n");
        preempt_enable();
        return 0;
    }

    mem_zero(process, sizeof(process_t));

    if (current_process && current_process->terminal) {
        /*
         * Nested process terminal sharing is kept safe for future process
         * creation paths. User apps should not spawn children directly yet.
         */
        process->terminal = current_process->terminal;
        process->owns_terminal = 0;
    } else {
        /*
         * KER-launched process: gets its own terminal.
         */
        process->terminal = terminal_create();
        process->owns_terminal = 1;

        if (!process->terminal) {
            kernel_print("process: terminal allocation failed\n");

            mem_zero(process, sizeof(process_t));
            preempt_enable();
            return 0;
        }
    }

    process->pid = next_pid++;
    process->kernel_stack_size = PROCESS_KERNEL_STACK_SIZE;
    process->parent_pid = current_process ? current_process->pid : 0;
    process->exit_code = 0;
    process->kill_requested = 0;
    process->has_started = 0;
    process->run_in_user_mode = 1;
    process->user_mode_ready = 0;
    str_copy(process->name, filename, sizeof(process->name));

    process->kernel_stack_base = kmalloc(process->kernel_stack_size);
    if (!process->kernel_stack_base) {
        kernel_print("process: stack allocation failed\n");

        if (process->owns_terminal && process->terminal) {
            terminal_destroy(process->terminal);
        }

        mem_zero(process, sizeof(process_t));
        preempt_enable();
        return 0;
    }

    /*
     * Create the process page directory after kernel-side allocations,
     * so the cloned kernel mappings include the allocated process stack
     * and terminal structures.
     */
    process->page_directory = paging_create_process_directory();
    if (!process->page_directory) {
        kernel_print("process: page directory allocation failed\n");

        if (process->kernel_stack_base) {
            kfree(process->kernel_stack_base);
        }

        if (process->owns_terminal && process->terminal) {
            terminal_destroy(process->terminal);
        }

        mem_zero(process, sizeof(process_t));
        preempt_enable();
        return 0;
    }

    if (!process_alloc_user_stack(process)) {
        kernel_print("process: user stack allocation failed\n");

        paging_destroy_process_directory(process->page_directory);

        if (process->kernel_stack_base) {
            kfree(process->kernel_stack_base);
        }

        if (process->owns_terminal && process->terminal) {
            terminal_destroy(process->terminal);
        }

        mem_zero(process, sizeof(process_t));
        preempt_enable();
        return 0;
    }

    if (!exec_load_into_directory(filename, &process->image, process->page_directory)) {
        process_free_user_stack(process);

        if (process->kernel_stack_base) {
            kfree(process->kernel_stack_base);
        }

        if (process->owns_terminal && process->terminal) {
            terminal_destroy(process->terminal);
        }

        paging_destroy_process_directory(process->page_directory);

        mem_zero(process, sizeof(process_t));
        preempt_enable();
        return 0;
    }

    process->user_mode_ready = 1;
    process->state = PROCESS_STATE_READY;

    preempt_enable();
    return process;
}

/*
 * Blocking/single-run path for user-mode ELF programs.
 */
int process_run(process_t* process) {
    if (!process || process->state != PROCESS_STATE_READY) {
        return 0;
    }

    process->run_in_user_mode = 1;

    while (process->state == PROCESS_STATE_READY ||
           process->state == PROCESS_STATE_RUNNING) {
        process_schedule_once();
    }

    return process->exit_code == 0;
}

void process_destroy(process_t* process) {
    preempt_disable();

    if (!process || process->state == PROCESS_STATE_UNUSED) {
        preempt_enable();
        return;
    }

    if (foreground_pid == process->pid) {
        foreground_pid = 0;
    }

    exec_unload(&process->image);

    if (process->kernel_stack_base) {
        kfree(process->kernel_stack_base);
    }

    process->kernel_stack_base = 0;
    process->kernel_stack_size = 0;

    if (process->page_directory) {
        process_free_user_stack(process);
        paging_destroy_process_directory(process->page_directory);
        process->page_directory = 0;
    }

    process->state = PROCESS_STATE_EXITED;

    preempt_enable();
}

void process_reap(process_t* process) {
    preempt_disable();

    if (!process || process->state != PROCESS_STATE_EXITED) {
        preempt_enable();
        return;
    }

    if (process->owns_terminal && process->terminal) {
        terminal_destroy(process->terminal);
    }

    process->terminal = 0;
    process->owns_terminal = 0;

    mem_zero(process, sizeof(process_t));

    preempt_enable();
}

void process_reap_all_exited(void) {
    preempt_disable();

    for (u32 i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (process_table[i].state == PROCESS_STATE_EXITED) {
            process_reap(&process_table[i]);
        }
    }

    preempt_enable();
}

process_t* process_find_by_pid(u32 pid) {
    for (u32 i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED &&
            process_table[i].pid == pid) {
            return &process_table[i];
        }
    }

    return 0;
}

process_t* process_current(void) {
    return current_process;
}

u32 process_current_pid(void) {
    return current_process ? current_process->pid : 0;
}

u32 process_foreground_pid(void) {
    return foreground_pid;
}

int process_set_foreground(u32 pid) {
    process_t* p;

    preempt_disable();

    p = process_find_by_pid(pid);
    if (!p) {
        preempt_enable();
        return 0;
    }

    if (p->state == PROCESS_STATE_UNUSED ||
        p->state == PROCESS_STATE_EXITED) {
        preempt_enable();
        return 0;
    }

    foreground_pid = pid;

    preempt_enable();
    return 1;
}

void process_clear_foreground(void) {
    preempt_disable();
    foreground_pid = 0;
    preempt_enable();
}

u32 process_count_active(void) {
    u32 count = 0;

    for (u32 i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED) {
            count++;
        }
    }

    return count;
}

process_t* process_table_at(u32 index) {
    if (index >= PROCESS_MAX_COUNT) {
        return 0;
    }

    return &process_table[index];
}

int process_request_kill(u32 pid) {
    preempt_disable();

    process_t* p = process_find_by_pid(pid);

    if (!p) {
        preempt_enable();
        return 0;
    }

    if (p->state == PROCESS_STATE_UNUSED || p->state == PROCESS_STATE_EXITED) {
        preempt_enable();
        return 0;
    }

    if (p->pid == 0) {
        preempt_enable();
        return 0;
    }

    p->kill_requested = 1;

    if (p == current_process) {
        kill_current_process_requested = 1;
    }

    preempt_enable();
    return 1;
}

int process_kill_requested(void) {
    if (!current_process) {
        return 0;
    }

    return kill_current_process_requested || current_process->kill_requested;
}

void process_clear_kill_request(void) {
    kill_current_process_requested = 0;

    if (current_process) {
        current_process->kill_requested = 0;
    }
}

int process_pause(u32 pid) {
    process_t* p;

    preempt_disable();

    p = process_find_by_pid(pid);
    if (!p) {
        preempt_enable();
        return 0;
    }

    if (p->state == PROCESS_STATE_UNUSED ||
        p->state == PROCESS_STATE_EXITED) {
        preempt_enable();
        return 0;
    }

    if (p->state == PROCESS_STATE_PAUSED) {
        preempt_enable();
        return 1;
    }

    /*
     * If the process is currently running, we mark it PAUSED.
     * The active preemption/yield path must preserve that state
     * instead of changing it back to READY.
     */
    p->state = PROCESS_STATE_PAUSED;

    preempt_enable();
    return 1;
}

int process_resume(u32 pid) {
    process_t* p;

    preempt_disable();

    p = process_find_by_pid(pid);
    if (!p) {
        preempt_enable();
        return 0;
    }

    if (p->state != PROCESS_STATE_PAUSED) {
        preempt_enable();
        return 0;
    }

    p->state = PROCESS_STATE_READY;

    preempt_enable();
    return 1;
}

void process_yield(void) {
    process_t* self;

    if (!current_process) {
        return;
    }

    self = current_process;

    if (process_kill_requested() || self->kill_requested) {
        self->exit_code = -2;
        self->state = PROCESS_STATE_EXITED;
    }
    else if (process_foreground_escape_requested() &&
             process_foreground_pid() == self->pid) {
        self->state = PROCESS_STATE_PAUSED;
        process_clear_foreground();
        process_clear_foreground_escape_request();
        terminal_switch(terminal_kernel());
    }
    else if (self->state != PROCESS_STATE_PAUSED) {
        self->state = PROCESS_STATE_READY;
    }

    current_process = 0;

    context_switch(&self->context.esp, kernel_context_esp);
}

void process_exit_current(int exit_code) {
    process_t* self;

    if (!current_process) {
        return;
    }

    self = current_process;

    self->exit_code = exit_code;
    self->state = PROCESS_STATE_EXITED;

    process_clear_kill_request();
    current_process = 0;

    context_switch(&self->context.esp, kernel_context_esp);

    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

void process_preempt_from_irq(void) {
    process_t* self;

    if (!current_process) {
        return;
    }

    self = current_process;

    if (process_kill_requested()) {
        self->exit_code = -2;
        self->state = PROCESS_STATE_EXITED;
    }
    else if (process_foreground_escape_requested() &&
             process_foreground_pid() == self->pid) {
        self->state = PROCESS_STATE_PAUSED;
        process_clear_foreground();
        process_clear_foreground_escape_request();
        terminal_switch(terminal_kernel());
    }
    else if (self->state != PROCESS_STATE_PAUSED) {
        self->state = PROCESS_STATE_READY;
    }

    current_process = 0;

    context_switch(&self->context.esp, kernel_context_esp);
}

void process_schedule_if_requested(void) {
    if (!schedule_requested) {
        return;
    }

    if (!preempt_can_switch()) {
        return;
    }

    schedule_requested = 0;

    if (!process_current()) {
        return;
    }

    process_yield();
}

int process_has_runnable(void) {
    for (u32 i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (process_table[i].state == PROCESS_STATE_READY) {
            return 1;
        }
    }

    return 0;
}

int process_set_user_mode(process_t* process, int enabled) {
    if (!process) {
        return 0;
    }

    process->run_in_user_mode = enabled ? 1 : 0;
    return 1;
}

void process_attach_terminal(process_t* process, terminal_session_t* terminal, int owns_terminal) {
    if (!process) {
        return;
    }

    if (process->owns_terminal && process->terminal && process->terminal != terminal) {
        terminal_destroy(process->terminal);
    }

    process->terminal = terminal;
    process->owns_terminal = owns_terminal ? 1 : 0;
}

int process_schedule_once(void) {
    process_t* next;

    preempt_disable();

    next = process_select_next();

    if (!next) {
        preempt_enable();
        return 0;
    }

    if (!next->has_started) {
        process_prepare_initial_stack(next);
    }

    if (!next->run_in_user_mode) {
        kernel_print("process: refusing to run non-user-mode process\n");
        next->exit_code = -1;
        next->state = PROCESS_STATE_EXITED;
        preempt_enable();
        return 1;
    }

    if (!next->page_directory) {
        next->exit_code = -1;
        next->state = PROCESS_STATE_EXITED;
        preempt_enable();
        return 1;
    }

    next->state = PROCESS_STATE_RUNNING;
    current_process = next;
    process_clear_kill_request();

    tss_set_kernel_stack(process_kernel_stack_top(next));

    /*
     * Switch to the process address space before switching to
     * the process stack/context.
     */
    paging_switch_directory(next->page_directory);

    preempt_enable();

    context_switch(&kernel_context_esp, next->context.esp);

    /*
     * If we returned here from IRQ-preemption, interrupts may still be off
     * because we escaped IRQ context before iretd restored EFLAGS.
     */
    __asm__ volatile ("sti");

    preempt_disable();

    /*
     * Back in scheduler/kernel context. Use the kernel page directory again
     * before freeing process resources.
     */
    paging_switch_kernel_directory();

    if (next->state == PROCESS_STATE_EXITED) {
        process_destroy(next);
    }

    current_process = 0;

    preempt_enable();
    return 1;
}