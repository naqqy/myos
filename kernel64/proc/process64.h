#ifndef PROCESS64_H
#define PROCESS64_H

#include "naq_boot_info.h"
#include "../console/terminal64.h"

#define PROCESS64_MAX_TASKS 24
#define PROCESS64_STACK_SIZE (256u * 1024u)
#define PROCESS64_INPUT_QUEUE_SIZE 128

/*
 * Spawn/terminal policy flags.
 *
 * FOREGROUND:
 *   The process is treated as foreground and shares the current terminal.
 *
 * PRIVATE_TERMINAL:
 *   The process gets its own private terminal/session. Use view <pid> to see it.
 *
 * If neither flag is set, the default is to share the current terminal.
 */
#define PROCESS64_SPAWN_FOREGROUND       0x01u
#define PROCESS64_SPAWN_PRIVATE_TERMINAL 0x02u

#define PROCESS64_MAX_FILES 16

#define PROCESS64_GFX_NONE   0
#define PROCESS64_GFX_ACTIVE 1

#define PROCESS64_GFX_MAX_WIDTH  640
#define PROCESS64_GFX_MAX_HEIGHT 480

typedef struct process64_gfx {
    int mode;

    nq_u32 width;
    nq_u32 height;

    nq_u32 frame_width;
    nq_u32 frame_height;

    int dst_x;
    int dst_y;

    nq_u32* buffer;
    nq_u64 buffer_size;
    int dirty;
} process64_gfx_t;

typedef struct process64_file {
    int used;
    char path[128];
    nq_u64 size;
    nq_u64 offset;
} process64_file_t;

typedef struct process64_user_memory {
    /*
     * POC user memory swap support.
     *
     * Real solution later:
     *   per-process page tables / address spaces.
     *
     * Current solution:
     *   all ELF apps still execute at the same fixed user virtual range,
     *   so we save/restore the loaded image range and user stack when the
     *   scheduler switches between user apps.
     */
    int active;

    nq_u64 image_base;
    nq_u64 image_size;
    void* image_backup;

    nq_u64 stack_base;
    nq_u64 stack_size;
    void* stack_backup;
} process64_user_memory_t;

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

    /*
     * Address space:
     * kernel tasks use paging64_kernel_pml4().
     * ELF/user tasks get their own PML4.
     */
    nq_u64 pml4_phys;
    int owns_address_space;

    process64_entry_t entry;
    void* arg;

    terminal64_session_t* terminal;
    nq_u32 terminal_id;
    int foreground;
    int owns_terminal;

    char name[32];

    char input_queue[PROCESS64_INPUT_QUEUE_SIZE];
    nq_u32 input_head;
    nq_u32 input_tail;

    process64_file_t files[PROCESS64_MAX_FILES];

    process64_gfx_t gfx;

    process64_user_memory_t user;
} process64_task_t;

void process64_init(void);

int process64_create_kernel_task(
    const char* name,
    process64_entry_t entry,
    void* arg
);

int process64_create_kernel_task_ex(
    const char* name,
    process64_entry_t entry,
    void* arg,
    nq_u32 flags
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

int process64_current_create_user_address_space(void);
nq_u64 process64_current_pml4(void);


void process64_print_summary(void);

terminal64_session_t* process64_terminal(process64_task_t* task);
terminal64_session_t* process64_current_terminal(void);

int process64_view_terminal(nq_u32 pid);
int process64_foreground(nq_u32 pid);

void process64_write_current(const char* s);
void process64_write_current_len(const char* s, nq_u64 len);

nq_u32 process64_current_pid(void);
void process64_sleep_ticks(nq_u64 ticks);

int process64_is_alive(nq_u32 pid);
void process64_wait_pid(nq_u32 pid);

int process64_has_foreground_app(void);
nq_u32 process64_foreground_pid(void);
int process64_current_is_foreground(void);

int process64_push_key_to_foreground(char c);
int process64_input_poll_key(void);
int process64_input_get_key(void);

void process64_reap_zombies(void);
int process64_reap_pid(nq_u32 pid);
nq_u32 process64_zombie_count(void);

int process64_file_open(const char* path, nq_u64 size);
int process64_file_read(int handle, void* dst, nq_u64 max_bytes);
int process64_file_close(int handle);
process64_file_t* process64_file_from_handle(int handle);

int process64_user_memory_setup(
    nq_u64 image_base,
    nq_u64 image_size,
    nq_u64 stack_base,
    nq_u64 stack_size
);

void process64_user_memory_save_current(void);
void process64_user_memory_restore_current(void);

int process64_set_graphics_mode(nq_u32 mode);
nq_u32 process64_get_graphics_mode(void);

int process64_present_current_gfx(
    const nq_u32* pixels,
    nq_u32 width,
    nq_u32 height,
    int dst_x,
    int dst_y
);

int process64_show_graphics(nq_u32 pid);
int process64_task_has_graphics(nq_u32 pid);

#endif
