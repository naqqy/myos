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

#include "../graphics/framebuffer64.h"
#include "../graphics/image64.h"

#include "../console/terminal64.h"

#include "../proc/process64.h"

#define SHELL64_INPUT_MAX 128
#define SHELL64_FILE_BUFFER_SIZE 65536u
#define SHELL64_IMAGE_BUFFER_SIZE (1024u * 1024u)

static nq_u8 g_image_buffer[SHELL64_IMAGE_BUFFER_SIZE];
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

static nq_u64 shell64_strlen(const char* s) {
    nq_u64 len = 0;

    if (!s) {
        return 0;
    }

    while (s[len]) {
        len++;
    }

    return len;
}

static int shell64_parse_echo_append(
    const char* arg,
    const char** out_text,
    nq_u64* out_text_len,
    const char** out_path
) {
    const char* text_start;
    const char* text_end;
    const char* p;

    if (!arg || !out_text || !out_text_len || !out_path) {
        return 0;
    }

    *out_text = 0;
    *out_text_len = 0;
    *out_path = 0;

    if (!str_starts_with(arg, "echo ")) {
        return 0;
    }

    p = skip_spaces(arg + 5);

    /*
     * Supports:
     *   echo "hello world" >> /LOG.TXT
     *   echo hello >> /LOG.TXT
     */
    if (*p == '"') {
        p++;
        text_start = p;

        while (*p && *p != '"') {
            p++;
        }

        if (*p != '"') {
            return 0;
        }

        text_end = p;
        p++;
    } else {
        text_start = p;

        while (*p) {
            if (p[0] == '>' && p[1] == '>') {
                break;
            }

            p++;
        }

        text_end = p;

        /*
         * Trim spaces before >>.
         */
        while (text_end > text_start &&
               (text_end[-1] == ' ' || text_end[-1] == '\t')) {
            text_end--;
        }
    }

    p = skip_spaces(p);

    if (p[0] != '>' || p[1] != '>') {
        return 0;
    }

    p += 2;
    p = skip_spaces(p);

    if (!*p) {
        return 0;
    }

    *out_text = text_start;
    *out_text_len = (nq_u64)(text_end - text_start);
    *out_path = p;

    return 1;
}

static void shell64_echo_append(const char* arg) {
    const char* text;
    const char* path;
    nq_u64 text_len = 0;
    nq_u64 old_size = 0;
    nq_u64 new_size = 0;

    if (!shell64_parse_echo_append(arg, &text, &text_len, &path)) {
        k64_print("Usage: echo \"TEXT\" >> <path>\n");
        return;
    }

    /*
     * Reuse g_file_buffer as the append/write buffer.
     * Behavior:
     *   existing file found -> append text + newline
     *   file missing        -> create file with text + newline
     */
    if (!vfs64_read_file(path, g_file_buffer, SHELL64_FILE_BUFFER_SIZE, &old_size)) {
        old_size = 0;
    }

    new_size = old_size + text_len + 1u;

    if (new_size > SHELL64_FILE_BUFFER_SIZE) {
        k64_print("echo: file would exceed shell buffer size.\n");
        return;
    }

    for (nq_u64 i = 0; i < text_len; i++) {
        g_file_buffer[old_size + i] = (nq_u8)text[i];
    }

    g_file_buffer[old_size + text_len] = '\n';

    if (!vfs64_write_file(path, g_file_buffer, new_size)) {
        k64_print("echo: write failed. New files currently need 8.3 names.\n");
        return;
    }

    k64_print("Wrote ");
    k64_print_dec64(new_size);
    k64_print(" bytes to ");
    k64_print(path);
    k64_print("\n");
}

static void shell64_prompt(void) {
    k64_print("KER64 > ");
}

static void shell64_print_help(void) {
    k64_print("Commands:\n");
    k64_print("  help          - show this help\n");
    k64_print("  about         - show kernel info\n");
    k64_print("  clear / cls   - clear screen\n");
    k64_print("  mkdir <path>  - create directory\n");
    k64_print("  rm <path>     - remove file\n");
    k64_print("  rmdir <path>  - remove empty directory\n");
    k64_print("  echo \"TEXT\" >> <path> - append text to file\n");
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
    k64_print("  fb            - show framebuffer info\n");
    k64_print("  nimg <file>   - draw centered NIMG image\n");
    k64_print("  splash        - draw /boot/splash.ni if present\n");
    k64_print("  term          - show terminal/session info\n");
    k64_print("  ps            - show kernel tasks\n");
    k64_print("  proctest      - create two cooperative test tasks\n");
    k64_print("  preempton     - enable timer preemption\n");
    k64_print("  preemptoff    - disable timer preemption\n");
    k64_print("  schedstat     - show scheduler status\n");
    k64_print("  busytest      - create CPU-bound preemption test tasks\n");
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

static void shell64_draw_nimg(const char* path) {
    path = skip_spaces(path);

    if (!path || !path[0]) {
        k64_print("Usage: nimg <file>\n");
        return;
    }

    if (!image64_draw_nimg_file_centered(
            path,
            g_image_buffer,
            SHELL64_IMAGE_BUFFER_SIZE)) {
        k64_print("nimg: image load/draw failed.\n");
        return;
    }

    /*
     * Image drawing overwrites the framebuffer directly.
     * Put the prompt area back into a clean state afterwards.
     */
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

static volatile nq_u64 g_busy_sink = 0;

static void shell64_busy_task_a(void* arg) {
    (void)arg;

    for (nq_u32 round = 0; round < 12; round++) {
        for (nq_u64 i = 0; i < 8000000ull; i++) {
            g_busy_sink += i;
        }

        k64_print("[busy A] round ");
        k64_print_dec64(round);
        k64_print("\n");
    }
}

static void shell64_busy_task_b(void* arg) {
    (void)arg;

    for (nq_u32 round = 0; round < 12; round++) {
        for (nq_u64 i = 0; i < 8000000ull; i++) {
            g_busy_sink += i;
        }

        k64_print("[busy B] round ");
        k64_print_dec64(round);
        k64_print("\n");
    }
}

static void shell64_start_busy_test(void) {
    int a = process64_create_kernel_task(
        "busy-a",
        shell64_busy_task_a,
        0
    );

    int b = process64_create_kernel_task(
        "busy-b",
        shell64_busy_task_b,
        0
    );

    k64_print("Created busy task A pid=");
    k64_print_dec64((nq_u64)a);
    k64_print("\n");

    k64_print("Created busy task B pid=");
    k64_print_dec64((nq_u64)b);
    k64_print("\n");

    k64_print("Run 'preempton' to let timer IRQs preempt them.\n");
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

static void shell64_test_task_a(void* arg) {
    (void)arg;

    for (nq_u32 i = 0; i < 8; i++) {
        k64_print("[task A] tick ");
        k64_print_dec64(i);
        k64_print("\n");

        process64_yield();
    }
}

static void shell64_test_task_b(void* arg) {
    (void)arg;

    for (nq_u32 i = 0; i < 8; i++) {
        k64_print("[task B] tick ");
        k64_print_dec64(i);
        k64_print("\n");

        process64_yield();
    }
}

static void shell64_start_process_test(void) {
    int a = process64_create_kernel_task(
        "test-a",
        shell64_test_task_a,
        0
    );

    int b = process64_create_kernel_task(
        "test-b",
        shell64_test_task_b,
        0
    );

    k64_print("Created task A pid=");
    k64_print_dec64((nq_u64)a);
    k64_print("\n");

    k64_print("Created task B pid=");
    k64_print_dec64((nq_u64)b);
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
        const char* text = 0;
        const char* path = 0;
        nq_u64 text_len = 0;

        if (shell64_parse_echo_append(arg, &text, &text_len, &path)) {
            shell64_echo_append(arg);
        } else {
            k64_print(skip_spaces(arg + 5));
            k64_print("\n");
        }
    }
    else if (str_eq(arg, "echo")) {
        k64_print("\n");
    }
    else if (str_starts_with(arg, "mkdir ")) {
        const char* path = skip_spaces(arg + 6);

        if (!path[0]) {
            k64_print("Usage: mkdir <path>\n");
        } else if (vfs64_mkdir(path)) {
            k64_print("Directory created.\n");
        } else {
            k64_print("mkdir failed. New directories currently need 8.3 names.\n");
        }
    }
    else if (str_starts_with(arg, "rm ")) {
        const char* path = skip_spaces(arg + 3);

        if (!path[0]) {
            k64_print("Usage: rm <path>\n");
        } else if (vfs64_rm(path)) {
            k64_print("File removed.\n");
        } else {
            k64_print("rm failed.\n");
        }
    }
    else if (str_starts_with(arg, "rmdir ")) {
        const char* path = skip_spaces(arg + 6);

        if (!path[0]) {
            k64_print("Usage: rmdir <path>\n");
        } else if (vfs64_rmdir(path)) {
            k64_print("Directory removed.\n");
        } else {
            k64_print("rmdir failed. Directory must be empty.\n");
        }
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
    else if (str_eq(arg, "fb")) {
        fb64_print_summary();
    }
    else if (str_starts_with(arg, "nimg ")) {
        shell64_draw_nimg(arg + 5);
    }
    else if (str_eq(arg, "splash")) {
        shell64_draw_nimg("/boot/splash.ni");
    }
    else if (str_eq(arg, "term")) {
        terminal64_print_summary();
    }
    else if (str_eq(arg, "ps")) {
    process64_print_summary();
    }
    else if (str_eq(arg, "proctest")) {
        shell64_start_process_test();
    }
    else if (str_eq(arg, "preempton")) {
        process64_enable_preemption();
        k64_print("Timer preemption enabled.\n");
    }
    else if (str_eq(arg, "preemptoff")) {
        process64_disable_preemption();
        k64_print("Timer preemption disabled.\n");
    }
    else if (str_eq(arg, "schedstat")) {
        process64_print_summary();
    }
    else if (str_eq(arg, "busytest")) {
        shell64_start_busy_test();
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
    else if (str_starts_with(arg, "run ")) {
        shell64_not_ready("run");
    }
    else if (str_starts_with(arg, "spawn ")) {
        shell64_not_ready("spawn");
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