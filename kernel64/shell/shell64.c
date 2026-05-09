#include "shell64.h"

#include "../k64_console.h"
#include "../input/kinput64.h"

#include "../arch/x86_64/power64.h"
#include "../arch/x86_64/timer64.h"

#include "../mm/memmap64.h"
#include "../mm/pmm64.h"
#include "../mm/kmem64.h"
#include "../mm/paging64.h"

#include "../storage/block64.h"

#include "../fs/vfs64.h"

#define SHELL64_INPUT_MAX 128
#define SHELL64_FILE_BUFFER_SIZE 65536u

static nq_u8 g_file_buffer[SHELL64_FILE_BUFFER_SIZE];

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

static const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }

    return s;
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
    k64_print("  help          - show this help\n");
    k64_print("  about         - show kernel info\n");
    k64_print("  clear / cls   - clear screen\n");
    k64_print("  echo <text>   - print text\n");
    k64_print("  ls            - list root directory\n");
    k64_print("  ls <path>     - list directory/file\n");
    k64_print("  cat <file>    - print text file\n");
    k64_print("  panic         - trigger invalid opcode exception\n");
    k64_print("  ticks         - show timer ticks\n");
    k64_print("  uptime        - show uptime in seconds\n");
    k64_print("  mem           - show memory map + PMM summary\n");
    k64_print("  pmm           - show physical memory manager summary\n");
    k64_print("  paging        - show paging summary\n");
    k64_print("  heap          - show heap summary\n");
    k64_print("  heaptest      - allocate/free test blocks\n");
    k64_print("  block         - show block device info\n");
    k64_print("  blockdump <n> - dump first 128 bytes of sector n\n");
    k64_print("  fs            - show filesystem info\n");
    k64_print("  norm <path>   - show normalized VFS path\n");
    k64_print("  reboot        - reboot machine\n");
    k64_print("  shutdown      - power off emulator\n");
}

static void shell64_norm_path(const char* path) {
    char normalized[VFS64_MAX_PATH];

    path = skip_spaces(path);

    if (!vfs64_normalize_path(path, normalized, sizeof(normalized))) {
        k64_print("norm: invalid or too-long path\n");
        return;
    }

    k64_print(normalized);
    k64_print("\n");
}

static void shell64_print_about(void) {
    k64_print("NAQ OS kernel64\n");
    k64_print("Boot path: UEFI x86_64\n");
    k64_print("Shell: KER64\n");
    k64_print("Status: framebuffer, GDT, IDT, IRQ, timer, keyboard, PMM, paging, heap\n");
}

static void shell64_print_ticks(void) {
    k64_print("Ticks: ");
    k64_print_dec64(timer64_get_ticks());
    k64_print("\n");

    k64_print("Frequency: ");
    k64_print_dec64(timer64_get_frequency());
    k64_print(" Hz\n");
}

static void shell64_print_uptime(void) {
    nq_u64 hz = timer64_get_frequency();
    nq_u64 ticks = timer64_get_ticks();

    k64_print("Uptime: ");

    if (hz == 0) {
        k64_print("unknown\n");
        return;
    }

    k64_print_dec64(ticks / hz);
    k64_print(" seconds\n");
}

static int parse_u64_shell(const char* s, nq_u64* out) {
    nq_u64 value = 0;
    int any = 0;
    int base = 10;

    if (!s || !out) {
        return 0;
    }

    s = skip_spaces(s);

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    while (*s) {
        char c = *s;
        nq_u64 digit;

        if (c >= '0' && c <= '9') {
            digit = (nq_u64)(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = (nq_u64)(10 + c - 'a');
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = (nq_u64)(10 + c - 'A');
        } else if (c == ' ' || c == '\t') {
            break;
        } else {
            return 0;
        }

        if (digit >= (nq_u64)base) {
            return 0;
        }

        value = value * (nq_u64)base + digit;
        any = 1;
        s++;
    }

    if (!any) {
        return 0;
    }

    *out = value;
    return 1;
}

static void shell64_print_hex8(nq_u8 value) {
    static const char hex[] = "0123456789ABCDEF";

    k64_putchar(hex[(value >> 4) & 0x0F]);
    k64_putchar(hex[value & 0x0F]);
}

static void shell64_blockdump(const char* arg) {
    static nq_u8 sector[BLOCK64_SECTOR_SIZE];
    nq_u64 lba = 0;

    if (!parse_u64_shell(arg, &lba)) {
        k64_print("Usage: blockdump <lba>\n");
        return;
    }

    if (!block64_available()) {
        k64_print("No block device available.\n");
        return;
    }

    if (!block64_read(lba, sector, 1)) {
        k64_print("blockdump: read failed.\n");
        return;
    }

    k64_print("Sector ");
    k64_print_dec64(lba);
    k64_print(" first 128 bytes:\n");

    for (nq_u32 i = 0; i < 128; i++) {
        if ((i % 16) == 0) {
            k64_print("\n");
            k64_print_hex64(i);
            k64_print(": ");
        }

        shell64_print_hex8(sector[i]);
        k64_putchar(' ');
    }

    k64_print("\n");
}

static void shell64_cat_file(const char* path) {
    nq_u64 size = 0;

    path = skip_spaces(path);

    if (!path || !path[0]) {
        k64_print("Usage: cat <file>\n");
        return;
    }

    if (!vfs64_read_file(
            path,
            g_file_buffer,
            SHELL64_FILE_BUFFER_SIZE,
            &size)) {
        k64_print("cat: file not found, too large, or read failed.\n");
        return;
    }

    for (nq_u64 i = 0; i < size; i++) {
        char c = (char)g_file_buffer[i];

        if (c == '\r') {
            continue;
        }

        if (c == '\n' || c == '\t' || (c >= 32 && c <= 126)) {
            k64_putchar(c);
        }
    }

    k64_print("\n");
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

static void shell64_not_ready(const char* name) {
    k64_print(name);
    k64_print(": not ported yet.\n");
}

static void shell64_execute_command(void) {
    const char* arg;

    g_input[g_input_len] = 0;

    arg = skip_spaces(g_input);

    if (*arg == 0) {
        return;
    }

    if (str_eq(arg, "help")) {
        shell64_print_help();
    }
    else if (str_eq(arg, "about") || str_eq(arg, "version")) {
        shell64_print_about();
    }
    else if (str_eq(arg, "clear") || str_eq(arg, "cls")) {
        k64_clear_screen();
    }
    else if (str_starts_with(arg, "echo ")) {
        k64_print(skip_spaces(arg + 5));
        k64_print("\n");
    }
    else if (str_eq(arg, "echo")) {
        k64_print("\n");
    }
    else if (str_eq(arg, "ls")) {
        vfs64_list_dir("/");
    }
    else if (str_starts_with(arg, "ls ")) {
        vfs64_list_dir(skip_spaces(arg + 3));
    }
    else if (str_starts_with(arg, "cat ")) {
        shell64_cat_file(arg + 4);
    }
    else if (str_eq(arg, "panic")) {
        k64_print("Triggering invalid opcode exception...\n");
        timer64_sleep_ms(250);
        __asm__ volatile ("ud2");
    }
    else if (str_eq(arg, "ticks")) {
        shell64_print_ticks();
    }
    else if (str_eq(arg, "uptime")) {
        shell64_print_uptime();
    }
    else if (str_eq(arg, "mem")) {
        memmap64_print_summary();
        pmm64_print_summary();
    }
    else if (str_eq(arg, "pmm")) {
        pmm64_print_summary();
    }
    else if (str_eq(arg, "paging")) {
        paging64_print_summary();
    }
    else if (str_eq(arg, "heap")) {
        kmem64_print_summary();
    }
    else if (str_eq(arg, "heaptest")) {
        shell64_heaptest();
    }
    else if (str_eq(arg, "block")) {
        block64_print_summary();
    }
    else if (str_starts_with(arg, "blockdump ")) {
        shell64_blockdump(arg + 10);
    }
    else if (str_eq(arg, "fs")) {
        vfs64_print_summary();
    }
    else if (str_starts_with(arg, "norm ")) {
        shell64_norm_path(arg + 5);
    }
    else if (str_eq(arg, "reboot")) {
        k64_print("Rebooting...\n");
        power64_reboot();
    }
    else if (str_eq(arg, "shutdown")) {
        k64_print("Shutting down...\n");
        power64_shutdown();
    }

    /*
     * Old-shell commands intentionally reserved for later ports.
     */
    else if (str_starts_with(arg, "mkdir ")) {
        shell64_not_ready("mkdir");
    }
    else if (str_starts_with(arg, "rm ")) {
        shell64_not_ready("rm");
    }
    else if (str_starts_with(arg, "rmdir ")) {
        shell64_not_ready("rmdir");
    }
    else if (str_starts_with(arg, "run ")) {
        shell64_not_ready("run");
    }
    else if (str_starts_with(arg, "spawn ")) {
        shell64_not_ready("spawn");
    }
    else if (str_eq(arg, "ps")) {
        shell64_not_ready("ps");
    }
    else if (str_eq(arg, "sched") || str_eq(arg, "schedstat")) {
        shell64_not_ready("scheduler");
    }
    else {
        k64_print("Unknown command: ");
        k64_print(arg);
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