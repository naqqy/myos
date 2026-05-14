#include "include/naqos.h"

int main(void) {
    puts("Input test app\n");
    puts("Type keys. Press q to quit.\n");

    for (;;) {
        int c = input_get_key();

        if (c == 0) {
            yield();
            continue;
        }

        puts("key: ");
        putchar(c);
        puts("\n");

        if (c == 'q' || c == 'Q') {
            break;
        }
    }

    puts("input test done\n");
    return 0;
}