#include "shell.h"
#include "fat32.h"
#include "power.h"
#include "timer.h"
#include "kmem.h"
#include "pmm.h"
#include "terminal.h"
#include "process.h"

typedef unsigned int u32;
typedef unsigned char u8;

extern void kernel_print(const char* s);
extern void kernel_put_char(char c);
extern void kernel_backspace(void);
extern void kernel_clear(void);

extern volatile int scheduler_enabled;
extern volatile int schedule_requested;

#define SHELL_INPUT_MAX 128
#define FILE_BUFFER_SIZE 65536

static const char* exit_reason(int code) {
    switch (code) {
        case 0:  return "ok";
        case -1: return "error";
        case -2: return "killed";
        default: return "unknown";
    }
}

static char input_buffer[SHELL_INPUT_MAX];
static u32 input_len = 0;
static u8 file_buffer[FILE_BUFFER_SIZE];

static volatile int command_ready = 0;

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }

        a++;
        b++;
    }

    return *a == 0 && *b == 0;
}

static int str_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) {
            return 0;
        }

        s++;
        prefix++;
    }

    return 1;
}

/*
 * Legacy hooks kept as harmless no-ops for keyboard/process code that may
 * still call them during Ctrl+A / foreground transitions.
 */
void shell_cancel_app_read_line(void) {
    input_len = 0;
    input_buffer[0] = 0;
}

void shell_force_kernel_input(void) {
    input_len = 0;
    input_buffer[0] = 0;
    command_ready = 0;
}

void shell_pause_app_input_for_ker(void) {
    input_len = 0;
    input_buffer[0] = 0;
    command_ready = 0;
}

void shell_resume_app_input_after_fg(void) {
}

static int parse_u32(const char* s, u32* out) {
    u32 value = 0;
    int any = 0;

    while (*s == ' ' || *s == '\t') {
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        any = 1;
        value = value * 10 + (u32)(*s - '0');
        s++;
    }

    if (!any) {
        return 0;
    }

    *out = value;
    return 1;
}

static void print_dec32_local(u32 value) {
    char buf[16];
    int i = 0;

    if (value == 0) {
        kernel_put_char('0');
        return;
    }

    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        kernel_put_char(buf[--i]);
    }
}

static void print_int32_local(int value) {
    if (value < 0) {
        kernel_put_char('-');
        print_dec32_local((u32)(-value));
    } else {
        print_dec32_local((u32)value);
    }
}

static const char* process_state_name(process_state_t state) {
    switch (state) {
        case PROCESS_STATE_UNUSED:  return "unused";
        case PROCESS_STATE_READY:   return "ready";
        case PROCESS_STATE_RUNNING: return "running";
        case PROCESS_STATE_BLOCKED: return "blocked";
        case PROCESS_STATE_PAUSED:  return "paused";
        case PROCESS_STATE_EXITED:  return "exited";
        default: return "unknown";
    }
}

static void shell_prompt(void) {
    kernel_print("KER > ");
}

static void build_name83(const char* in, char out[11]) {
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    int i = 0;
    int j = 0;

    while (in[i] && in[i] != '.' && j < 8) {
        char c = in[i];

        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }

        out[j++] = c;
        i++;
    }

    if (in[i] == '.') {
        i++;
        j = 8;

        while (in[i] && j < 11) {
            char c = in[i];

            if (c >= 'a' && c <= 'z') {
                c -= 32;
            }

            out[j++] = c;
            i++;
        }
    }
}

static int kernel_cat_file(const char* filename) {
    u32 size = 0;

    if (!fat32_read_file_path(filename, file_buffer, FILE_BUFFER_SIZE, &size)) {
        kernel_print("File not found or read failed.\n");
        return 0;
    }

    for (u32 i = 0; i < size; i++) {
        char c = (char)file_buffer[i];

        if (c == '\r') {
            continue;
        }

        if (c == '\n' || (c >= 32 && c <= 126) || c == '\t') {
            kernel_put_char(c);
        }
    }

    kernel_print("\n");
    return 1;
}

static void kernel_ls_files(const char* path) {
    if (!path || !path[0]) {
        fat32_list_path("/");
    } else {
        fat32_list_path(path);
    }
}

static int filename_has_extension(const char* s) {
    while (*s) {
        if (*s == '.') {
            return 1;
        }

        s++;
    }

    return 0;
}

static int path_has_separator(const char* s) {
    while (*s) {
        if (*s == '/' || *s == '\\') {
            return 1;
        }

        s++;
    }

    return 0;
}

static void build_elf_program_name(const char* input, char* out, u32 out_size) {
    u32 i = 0;

    if (out_size == 0) {
        return;
    }

    /*
     * Bare app names search /apps by default:
     *   sysinfo -> /apps/sysinfo.elf
     *
     * Explicit paths are respected:
     *   run /apps/sysinfo
     *   run apps/sysinfo
     */
    if (!path_has_separator(input)) {
        const char* prefix = "/apps/";

        while (prefix[i] && i < out_size - 1) {
            out[i] = prefix[i];
            i++;
        }
    }

    for (u32 j = 0; input[j] && i < out_size - 1; j++) {
        out[i++] = input[j];
    }

    out[i] = 0;

    if (!filename_has_extension(input)) {
        if (i + 4 < out_size) {
            out[i++] = '.';
            out[i++] = 'e';
            out[i++] = 'l';
            out[i++] = 'f';
            out[i] = 0;
        }
    }
}

static int kernel_run_user_program(const char* filename, int private_terminal) {
    process_t* process;
    int ok;

    process = process_spawn(filename);
    if (!process) {
        return 0;
    }

    process_set_user_mode(process, 1);

    if (!private_terminal) {
        process_attach_terminal(process, terminal_kernel(), 0);
    }

    kernel_print("Running user-mode PID ");
    print_dec32_local(process->pid);
    kernel_print(": ");
    kernel_print(process->name);
    kernel_print(private_terminal ? " [private terminal]\n" : " [KER terminal]\n");

    process_set_foreground(process->pid);

    if (private_terminal) {
        terminal_switch(process->terminal);
    } else {
        terminal_switch(terminal_kernel());
    }

    ok = process_run(process);

    terminal_switch(terminal_kernel());
    process_clear_foreground();

    kernel_print("User-mode process exited with ");
    print_int32_local(process->exit_code);
    kernel_print(" (");
    kernel_print(exit_reason(process->exit_code));
    kernel_print(")\n");

    return ok;
}

static int process_is_alive_and_foreground(u32 pid) {
    process_t* p = process_find_by_pid(pid);

    if (!p) {
        return 0;
    }

    if (p->state == PROCESS_STATE_UNUSED ||
        p->state == PROCESS_STATE_EXITED ||
        p->state == PROCESS_STATE_PAUSED) {
        return 0;
    }

    if (process_foreground_pid() != pid) {
        return 0;
    }

    return 1;
}

void shell_init(void) {
    input_len = 0;
    command_ready = 0;

    for (u32 i = 0; i < SHELL_INPUT_MAX; i++) {
        input_buffer[i] = 0;
    }

    kernel_print("Kernel shell ready.\n");
    kernel_print("Type 'help' for commands.\n");
    shell_prompt();
}

static void shell_execute_command(void) {
    if (input_len == 0) {
        command_ready = 0;
        input_buffer[0] = 0;
        shell_prompt();
        return;
    }

    if (str_eq(input_buffer, "help")) {
        kernel_print("Commands:\n");
        kernel_print("  help         - show commands\n");
        kernel_print("  clear        - clear screen\n");
        kernel_print("  panic        - trigger invalid opcode exception\n");
        kernel_print("  echo <text>  - print text\n");
        kernel_print("  reboot       - reboot the machine\n");
        kernel_print("  shutdown     - power off in emulator\n");
        kernel_print("  kmalloc      - allocate and free a heap test block\n");
        kernel_print("  framealloc   - allocate and free one physical frame\n");
        kernel_print("  heaptest     - test heap allocation/reclaim\n");
        kernel_print("  ls           - list FAT32 root files\n");
        kernel_print("  mkdir <path> - create directory\n");
        kernel_print("  rm <path>    - remove file\n");
        kernel_print("  rmdir <path> - remove empty directory\n");
        kernel_print("  cat <file>   - print file contents\n");
        kernel_print("  run <file>   - run user-mode ELF app\n");
        kernel_print("  <file>       - run user-mode ELF app\n");
        kernel_print("  sched        - run ready processes cooperatively\n");
        kernel_print("  spawn <file> - create a ready user-mode process\n");
        kernel_print("  ps           - list process table\n");
        kernel_print("  reap         - clear exited processes\n");
        kernel_print("  kill <pid>   - request process kill\n");
        kernel_print("  pid          - show current pid\n");
        kernel_print("  preempton    - enable timer preemption\n");
        kernel_print("  preemptoff   - disable timer preemption\n");
        kernel_print("  schedstat    - show scheduler status\n");
        kernel_print("  pause <pid>  - pause a process\n");
        kernel_print("  resume <pid> - resume a paused process\n");
        kernel_print("  fg <pid>     - bring process to foreground\n");
        kernel_print("  view <pid>   - view a process terminal\n");
    }
    else if (str_eq(input_buffer, "clear")) {
        kernel_clear();
    }
    else if (str_eq(input_buffer, "schedstat")) {
        kernel_print("Scheduler enabled: ");
        kernel_print(scheduler_enabled ? "yes\n" : "no\n");

        kernel_print("Schedule requested: ");
        kernel_print(schedule_requested ? "yes\n" : "no\n");

        kernel_print("Can preempt: ");
        kernel_print(preempt_can_switch() ? "yes\n" : "no\n");

        kernel_print("Current PID: ");
        print_dec32_local(process_current_pid());
        kernel_print("\n");

        kernel_print("Runnable: ");
        kernel_print(process_has_runnable() ? "yes\n" : "no\n");
    }
    else if (str_eq(input_buffer, "pid")) {
        if (process_current() != 0) {
            kernel_print("Current PID: ");
            print_dec32_local(process_current_pid());
            kernel_print("\n");
        } else {
            kernel_print("No user process running.\n");
        }
    }
    else if (str_eq(input_buffer, "ps")) {
        kernel_print("Processes:\n");

        for (u32 i = 0; i < PROCESS_MAX_COUNT; i++) {
            process_t* p = process_table_at(i);

            if (!p || p->state == PROCESS_STATE_UNUSED) {
                continue;
            }

            kernel_print("  slot=");
            print_dec32_local(i);

            kernel_print(" pid=");
            print_dec32_local(p->pid);

            kernel_print(" state=");
            kernel_print(process_state_name(p->state));

            kernel_print(" parent=");
            print_dec32_local(p->parent_pid);

            kernel_print(" exit=");
            print_int32_local(p->exit_code);

            kernel_print(" name=");
            kernel_print(p->name);

            kernel_print(" ustack=");
            print_dec32_local(p->user_stack_page_count);
            kernel_print("pg");

            if (process_current() == p) {
                kernel_print(" *");
            }

            if (process_foreground_pid() == p->pid) {
                kernel_print(" FG");
            }

            kernel_print("\n");
        }
    }
    else if (str_eq(input_buffer, "reap")) {
        process_reap_all_exited();
        kernel_print("Exited processes reaped.\n");
    }
    else if (str_starts_with(input_buffer, "kill ")) {
        u32 pid = 0;
        const char* s = input_buffer + 5;

        while (*s >= '0' && *s <= '9') {
            pid = pid * 10 + (u32)(*s - '0');
            s++;
        }

        if (process_request_kill(pid)) {
            kernel_print("Kill requested for PID ");
            print_dec32_local(pid);
            kernel_print("\n");
        } else {
            kernel_print("Could not kill PID ");
            print_dec32_local(pid);
            kernel_print("\n");
        }
    }
    else if (str_eq(input_buffer, "panic")) {
        kernel_print("Triggering invalid opcode exception...\n");
        timer_sleep_ms(2000);
        __asm__ volatile ("ud2");
    }
    else if (str_starts_with(input_buffer, "echo ")) {
        kernel_print(input_buffer + 5);
        kernel_print("\n");
    }
    else if (str_eq(input_buffer, "ls")) {
        kernel_ls_files("/");
    }
    else if (str_starts_with(input_buffer, "ls ")) {
        kernel_ls_files(input_buffer + 3);
    }
    else if (str_starts_with(input_buffer, "cat ")) {
        kernel_cat_file(input_buffer + 4);
    }
    else if (str_starts_with(input_buffer, "mkdir ")) {
        if (fat32_mkdir_path(input_buffer + 6)) {
            kernel_print("Directory created.\n");
        } else {
            kernel_print("mkdir failed.\n");
        }
    }
    else if (str_starts_with(input_buffer, "rm ")) {
        if (fat32_rm_path(input_buffer + 3)) {
            kernel_print("File removed.\n");
        } else {
            kernel_print("rm failed.\n");
        }
    }
    else if (str_starts_with(input_buffer, "rmdir ")) {
        if (fat32_rmdir_path(input_buffer + 6)) {
            kernel_print("Directory removed.\n");
        } else {
            kernel_print("rmdir failed.\n");
        }
    }
    else if (str_starts_with(input_buffer, "run -t ")) {
        char program_name[128];

        build_elf_program_name(input_buffer + 7, program_name, sizeof(program_name));
        kernel_run_user_program(program_name, 1);
    }
    else if (str_starts_with(input_buffer, "run ")) {
        char program_name[128];

        build_elf_program_name(input_buffer + 4, program_name, sizeof(program_name));
        kernel_run_user_program(program_name, 0);
    }
    else if (str_starts_with(input_buffer, "spawn ")) {
        char program_name[128];
        process_t* p;

        build_elf_program_name(input_buffer + 6, program_name, sizeof(program_name));

        p = process_spawn(program_name);

        if (!p) {
            kernel_print("Could not spawn process.\n");
        } else {
            process_set_user_mode(p, 1);

            kernel_print("Spawned user-mode PID ");
            print_dec32_local(p->pid);
            kernel_print(": ");
            kernel_print(p->name);
            kernel_print("\n");
        }
    }
    else if (str_eq(input_buffer, "sched")) {
        kernel_print("Running scheduler...\n");

        while (process_has_runnable()) {
            process_schedule_once();
        }

        kernel_print("No more runnable processes.\n");
    }
    else if (str_eq(input_buffer, "heaptest")) {
        kernel_print("Committed before: ");
        print_dec32_local(kmem_committed_size());
        kernel_print("\n");

        void* a = kmalloc(64);
        void* b = kmalloc(2048);
        void* c = kmalloc(8192);

        kernel_print("Committed after alloc: ");
        print_dec32_local(kmem_committed_size());
        kernel_print("\n");

        if (!a || !b || !c) {
            kernel_print("heaptest: allocation failed\n");
        } else {
            kernel_print("heaptest: allocations succeeded\n");
        }

        if (a) {
            kfree(a);
        }

        if (b) {
            kfree(b);
        }

        if (c) {
            kfree(c);
        }

        kernel_print("Committed after free: ");
        print_dec32_local(kmem_committed_size());
        kernel_print("\n");

        kernel_print("heaptest: frees succeeded\n");
    }
    else if (str_eq(input_buffer, "reboot")) {
        kernel_print("Rebooting...\n");
        kernel_reboot();
    }
    else if (str_eq(input_buffer, "preempton") || str_eq(input_buffer, "schedon")) {
        scheduler_enabled = 1;
        kernel_print("Timer preemption enabled.\n");
    }
    else if (str_eq(input_buffer, "preemptoff") || str_eq(input_buffer, "schedoff")) {
        scheduler_enabled = 0;
        kernel_print("Timer preemption disabled.\n");
    }
    else if (str_eq(input_buffer, "framealloc")) {
        u32 frame = pmm_alloc_frame();

        if (!frame) {
            kernel_print("No free frame available.\n");
        } else {
            kernel_print("Allocated frame successfully.\n");
            pmm_free_frame(frame);
            kernel_print("Freed frame.\n");
        }
    }
    else if (str_eq(input_buffer, "shutdown")) {
        kernel_print("Shutting down...\n");
        kernel_shutdown();
    }
    else if (str_eq(input_buffer, "kmalloc")) {
        void* p = kmalloc(256);

        if (!p) {
            kernel_print("kmalloc failed.\n");
        } else {
            kernel_print("kmalloc(256) succeeded.\n");
            kfree(p);
            kernel_print("kfree succeeded.\n");
        }
    }
    else if (str_starts_with(input_buffer, "pause ")) {
        u32 pid;

        if (!parse_u32(input_buffer + 6, &pid)) {
            kernel_print("Usage: pause <pid>\n");
        } else if (process_pause(pid)) {
            kernel_print("Paused PID ");
            print_dec32_local(pid);
            kernel_print("\n");
        } else {
            kernel_print("Could not pause PID ");
            print_dec32_local(pid);
            kernel_print("\n");
        }
    }
    else if (str_starts_with(input_buffer, "resume ")) {
        u32 pid;

        if (!parse_u32(input_buffer + 7, &pid)) {
            kernel_print("Usage: resume <pid>\n");
        } else if (process_resume(pid)) {
            kernel_print("Resumed PID ");
            print_dec32_local(pid);
            kernel_print("\n");
        } else {
            kernel_print("Could not resume PID ");
            print_dec32_local(pid);
            kernel_print("\n");
        }
    }
    else if (str_starts_with(input_buffer, "fg ")) {
        u32 pid;
        process_t* p;

        if (!parse_u32(input_buffer + 3, &pid)) {
            kernel_print("Usage: fg <pid>\n");
        } else {
            p = process_find_by_pid(pid);

            if (!p || p->state == PROCESS_STATE_UNUSED || p->state == PROCESS_STATE_EXITED) {
                kernel_print("No such active process.\n");
            } else {
                if (p->state == PROCESS_STATE_PAUSED) {
                    process_resume(pid);
                }

                if (!process_set_foreground(pid)) {
                    kernel_print("Could not foreground PID ");
                    print_dec32_local(pid);
                    kernel_print("\n");
                } else {
                    kernel_print("Foreground PID ");
                    print_dec32_local(pid);
                    kernel_print("\n");

                    terminal_switch(p->terminal);

                    while (process_is_alive_and_foreground(pid)) {
                        p = process_find_by_pid(pid);

                        if (!p) {
                            break;
                        }

                        if (p->state == PROCESS_STATE_PAUSED ||
                            p->state == PROCESS_STATE_EXITED ||
                            p->state == PROCESS_STATE_UNUSED) {
                            break;
                        }

                        __asm__ volatile ("sti");

                        if (!process_schedule_once()) {
                            break;
                        }
                    }

                    terminal_switch(terminal_kernel());
                    kernel_print("Returned to KER.\n");
                }
            }
        }
    }
    else if (str_starts_with(input_buffer, "view ")) {
        u32 pid;
        process_t* p;

        if (!parse_u32(input_buffer + 5, &pid)) {
            kernel_print("Usage: view <pid>\n");
        } else {
            p = process_find_by_pid(pid);

            if (!p || !p->terminal) {
                kernel_print("No terminal for that PID.\n");
            } else {
                kernel_print("Viewing terminal for PID ");
                print_dec32_local(pid);
                kernel_print(". Press Ctrl+A or run fg to return later.\n");

                terminal_switch(p->terminal);
            }
        }
    }
    else {
        char program_name[128];

        build_elf_program_name(input_buffer, program_name, sizeof(program_name));

        if (!kernel_run_user_program(program_name, 0)) {
            kernel_print("Unknown command: ");
            kernel_print(input_buffer);
            kernel_print("\n");
        }
    }

    input_len = 0;
    input_buffer[0] = 0;
    command_ready = 0;
    shell_prompt();
}

void shell_putc(char c) {
    if (c == '\n') {
        kernel_put_char('\n');
        command_ready = 1;
        return;
    }

    if (c == '\b') {
        if (input_len > 0) {
            input_len--;
            input_buffer[input_len] = 0;
            kernel_backspace();
        }

        return;
    }

    if (c < 32 || c > 126) {
        return;
    }

    if (input_len + 1 >= SHELL_INPUT_MAX) {
        return;
    }

    input_buffer[input_len++] = c;
    input_buffer[input_len] = 0;
    kernel_put_char(c);
}

void shell_poll(void) {
    if (command_ready) {
        shell_execute_command();
    }
}