#include "api.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int str_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* dst, const char* src, int max_len) {
    int i = 0;
    if (max_len <= 0) return;

    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static void trim_in_place(char* s) {
    int start = 0;
    int end = str_len(s);

    while (s[start] == ' ' || s[start] == '\t') {
        start++;
    }

    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        end--;
    }

    int i = 0;
    while (start < end) {
        s[i++] = s[start++];
    }
    s[i] = 0;
}

static int has_dot(const char* s) {
    for (int i = 0; s[i]; i++) {
        if (s[i] == '.') return 1;
    }
    return 0;
}

static void build_program_name(const char* input, char* out, int out_size) {
    int i = 0;

    if (out_size <= 0) {
        return;
    }

    if (has_dot(input)) {
        while (input[i] && i < out_size - 1) {
            out[i] = input[i];
            i++;
        }
    } else {
        while (input[i] && i < out_size - 5) {
            out[i] = input[i];
            i++;
        }
        out[i++] = '.';
        out[i++] = 'N';
        out[i++] = 'A';
        out[i++] = 'Q';
    }

    out[i] = 0;
}

void app_main(kernel_api_t* api) {
    char line[128];

    api->print("COM ready.\n");
    api->print("Type 'help' for commands.\n");

    for (;;) {
        api->print("COM > ");

        int len = api->read_line(line, sizeof(line));

        if (len == -2) {
            api->print("^C\n");
            return;
        }

        if (len <= 0) {
            api->print("\n");
            continue;
        }

        trim_in_place(line);

        if (line[0] == 0) {
            continue;
        }

        if (str_eq(line, "help")) {
            api->print("Commands:\n");
            api->print("  help       - show commands\n");
            api->print("  clear      - clear screen\n");
            api->print("  echo ...   - echo text\n");
            api->print("  ver        - show shell version\n");
            api->print("  reboot     - reboot system\n");
            api->print("  pid        - show current process PID\n");
            api->print("  shutdown   - power off in emulator\n");
            api->print("  exit       - return to kernel shell\n");
            api->print("  ls         - list files in root directory\n");
            api->print("  cat <file> - print file contents\n");
            api->print("  run <file> - run a .NAQ program\n");
        }
        else if (str_eq(line, "clear")) {
            api->clear();
        }
        else if (str_eq(line, "ver")) {
            api->print("COM version 0.1\n");
        }
        else if (str_eq(line, "pid")) {
            api->print("Current PID: ");
            api->print_dec32(api->current_pid());
            api->print("\n");
        }
        else if (str_eq(line, "exit")) {
            api->print("Leaving COM...\n");
            return;
        }
        else if (str_starts_with(line, "echo ")) {
            api->print(line + 5);
            api->print("\n");
        }
        else if (str_eq(line, "echo")) {
            api->print("\n");
        }
        else if (str_eq(line, "reboot")) {
            api->print("Rebooting...\n");
            api->reboot();
        }
        else if (str_eq(line, "shutdown")) {
            api->print("Shutting down...\n");
            api->shutdown();
        }
        else if (str_eq(line, "ls")) {
            api->ls();
        }
        else if (str_starts_with(line, "cat ")) {
            api->cat(line + 4);
        }
        else if (str_eq(line, "cat")) {
            api->print("Usage: cat <file>\n");
        }
        else if (str_starts_with(line, "run ")) {
            if (!api->run(line + 4)) {
                api->print("Could not run: ");
                api->print(line + 4);
                api->print("\n");
            }
        }
        else if (str_eq(line, "run")) {
            api->print("Usage: run <file>\n");
        }
        else {
            char program_name[128];
            build_program_name(line, program_name, sizeof(program_name));

            if (!api->run(program_name)) {
                api->print("Unknown command or program: ");
                api->print(line);
                api->print("\n");
            }
        }
    }
}