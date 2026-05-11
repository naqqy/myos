#ifndef PROCESS64_H
#define PROCESS64_H

#include "naq_boot_info.h"
#include "../console/terminal64.h"

#define PROCESS64_MAX_TASKS 16
#define PROCESS64_STACK_SIZE (32u * 1024u)

typedef void (*process64_entry_t)(void* arg);

typedef enum process64_state {
    PROCESS64_UNUSED = 0,
    PROCESS64_READY,
    PROCESS64_RUNNING,
    PROCESS64_ZOMBIE
} process64_state_t;

typedef struct process64_task {
    nq_u32 pid;
    process64_state_t state;

    nq_u64 rsp;

    nq_u64 stack_base;
    nq_u64 stack_size;

    process64_entry_t entry;
    void* arg;

    terminal64_session_t* terminal;
    nq_u32 terminal_id;
    int foreground;

    char name[32];
} process64_task_t;

void process64_init(void);

int process64_create_kernel_task(
    const char* name,
    process64_entry_t entry,
    void* arg
);

void process64_yield(void);
void process64_exit(void);
int process64_kill(nq_u32 pid);

void process64_enable_preemption(void);
void process64_disable_preemption(void);
int  process64_preemption_enabled(void);

void process64_on_timer_tick(void);
void process64_schedule(void);

process64_task_t* process64_current(void);

void process64_print_summary(void);

terminal64_session_t* process64_terminal(process64_task_t* task);
terminal64_session_t* process64_current_terminal(void);

int process64_view_terminal(nq_u32 pid);
int process64_foreground(nq_u32 pid);

void process64_write_current(const char* s);
void process64_write_current_len(const char* s, nq_u64 len);

#endif