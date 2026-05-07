#ifndef PROCESS_H
#define PROCESS_H

#include "exec.h"
#include "terminal.h"

typedef unsigned int u32;

#define PROCESS_MAX_COUNT 16

typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_BLOCKED,
    PROCESS_STATE_PAUSED,
    PROCESS_STATE_EXITED
} process_state_t;

typedef struct cpu_context {
    u32 esp;
} cpu_context_t;

typedef struct process {
    u32 pid;
    process_state_t state;

    exec_image_t image;

    void* kernel_stack_base;
    u32 kernel_stack_size;

    u32 parent_pid;
    int exit_code;
    int kill_requested;

    cpu_context_t context;
    int has_started;

    terminal_session_t* terminal;
    int owns_terminal;

    u32* page_directory;

    u32 user_stack_top;
    u32 user_stack_bottom;
    u32 user_stack_page_count;
    u32 user_stack_pages[8];

    int user_mode_ready;
    int run_in_user_mode;

    char name[32];
} process_t;

void process_table_init(void);

process_t* process_spawn(const char* filename);
int process_run(process_t* process);

void process_destroy(process_t* process);
void process_reap(process_t* process);
void process_reap_all_exited(void);

process_t* process_find_by_pid(u32 pid);
process_t* process_current(void);
u32 process_current_pid(void);

u32 process_count_active(void);
process_t* process_table_at(u32 index);

int process_request_kill(u32 pid);
int process_pause(u32 pid);
int process_resume(u32 pid);

u32 process_foreground_pid(void);
int process_set_foreground(u32 pid);
void process_clear_foreground(void);

int process_kill_requested(void);
void process_clear_kill_request(void);
int process_foreground_escape_requested(void);
void process_clear_foreground_escape_request(void);

void process_yield(void);
void process_exit_current(int exit_code);

int process_has_runnable(void);
int process_schedule_once(void);
void process_schedule_if_requested(void);
void process_preempt_from_irq(void);

void preempt_disable(void);
void preempt_enable(void);
int  preempt_can_switch(void);

int process_set_user_mode(process_t* process, int enabled);

void process_attach_terminal(process_t* process, terminal_session_t* terminal, int owns_terminal);

#endif