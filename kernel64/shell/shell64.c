#include "shell64.h"

#include "../k64_console.h"
#include "../input/kinput64.h"

#include "../arch/x86_64/power64.h"
#include "../arch/x86_64/timer64.h"

#include "../mm/memmap64.h"
#include "../mm/pmm64.h"
#include "../mm/kmem64.h"

#define SHELL64_INPUT_MAX 128

static char g_input[SHELL64_INPUT_MAX];
static unsigned int g_input_len = 0;

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

static void shell64_clear_input(void) {
    for (unsigned int i = 0; i < SHELL64_INPUT_MAX; i++) {
        g_input[i] = 0;
    }

    g_input_len = 0;
}

static void shell64_prompt(void) {
    k64_print("KER64 > ");
}

static void shell64_print_help(void) {
    k64_print("Commands:\n");
    k64_print("  help         - show commands\n");
    k64_print("  clear        - clear screen\n");
    k64_print("  echo <text>  - print text\n");
    k64_print("  panic        - trigger invalid opcode exception\n");
    k64_print("  ticks        - show timer ticks\n");
    k64_print("  mem          - show memory summaries\n");
    k64_print("  heap         - show heap summary\n");
    k64_print("  heaptest     - allocate/free test blocks\n");
    k64_print("  reboot       - reboot machine\n");
    k64_print("  shutdown     - power off emulator\n");
}

static void shell64_heaptest(void) {
    void* a = kmalloc64(64);
    void* b = kmalloc64(4096);
    void* c = kmalloc64(12345);

    k64_print("heaptest A: ");
    k64_print_hex64((nq_u64)a);
    k64_print("\n");

    k64_print("heaptest B: ");
    k64_print_hex64((nq_u64)b);
    k64_print("\n");

    k64_print("heaptest C: ");
    k64_print_hex64((nq_u64)c);
    k64_print("\n");

    if (a) {
        kfree64(a);
    }

    if (b) {
        kfree64(b);
    }

    if (c) {
        kfree64(c);
    }

    k64_print("heaptest complete.\n");
    kmem64_print_summary();
}

static void shell64_execute_command(void) {
    g_input[g_input_len] = 0;

    if (g_input_len == 0) {
        return;
    }

    if (str_eq(g_input, "help")) {
        shell64_print_help();
    }
    else if (str_eq(g_input, "clear")) {
        k64_clear_screen();
    }
    else if (str_starts_with(g_input, "echo ")) {
        k64_print(g_input + 5);
        k64_print("\n");
    }
    else if (str_eq(g_input, "panic")) {
        k64_print("Triggering invalid opcode exception...\n");
        timer64_sleep_ms(250);
        __asm__ volatile ("ud2");
    }
    else if (str_eq(g_input, "ticks")) {
        k64_print("Ticks: ");
        k64_print_dec64(timer64_get_ticks());
        k64_print("\n");

        k64_print("Frequency: ");
        k64_print_dec64(timer64_get_frequency());
        k64_print(" Hz\n");
    }
    else if (str_eq(g_input, "mem")) {
        memmap64_print_summary();
        pmm64_print_summary();
    }
    else if (str_eq(g_input, "heap")) {
        kmem64_print_summary();
    }
    else if (str_eq(g_input, "heaptest")) {
        shell64_heaptest();
    }
    else if (str_eq(g_input, "reboot")) {
        k64_print("Rebooting...\n");
        power64_reboot();
    }
    else if (str_eq(g_input, "shutdown")) {
        k64_print("Shutting down...\n");
        power64_shutdown();
    }
    else {
        k64_print("Unknown command: ");
        k64_print(g_input);
        k64_print("\n");
        k64_print("Type 'help' for commands.\n");
    }
}

void shell64_init(void) {
    shell64_clear_input();

    k64_print("Kernel64 shell ready.\n");
    k64_print("Type 'help' for commands.\n");

    shell64_prompt();
}

void shell64_put_char(char c) {
    if (c == '\r') {
        return;
    }

    if (c == '\n') {
        k64_putchar('\n');

        shell64_execute_command();
        shell64_clear_input();

        shell64_prompt();
        return;
    }

    if (c == '\b' || c == 127) {
        if (g_input_len > 0) {
            g_input_len--;
            g_input[g_input_len] = 0;
            k64_backspace();
        }

        return;
    }

    if (c == '\t') {
        c = ' ';
    }

    if (c < 32 || c > 126) {
        return;
    }

    if (g_input_len + 1 >= SHELL64_INPUT_MAX) {
        return;
    }

    g_input[g_input_len++] = c;
    g_input[g_input_len] = 0;

    k64_putchar(c);
}

void shell64_poll(void) {
    for (;;) {
        int c = kinput64_poll_char();

        if (c == 0) {
            break;
        }

        shell64_put_char((char)c);
    }
}