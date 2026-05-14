#include "include/naqos.h"

static void print_u64(u64 value) {
    char buf[32];
    int i = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        putchar(buf[--i]);
    }
}

int main(void) {
    char buf[256];
    naq_stat_t st;

    puts("File read test\n");

    if (stat("/docs/readme.txt", &st) < 0) {
        puts("stat failed\n");
        return 1;
    }

    puts("size=");
    print_u64(st.size);
    puts("\n");

    int fd = open("/docs/readme.txt", 0);

    if (fd < 0) {
        puts("open failed\n");
        return 1;
    }

    i64 n = read(fd, buf, sizeof(buf) - 1);

    if (n < 0) {
        puts("read failed\n");
        close(fd);
        return 1;
    }

    buf[n] = 0;

    puts("contents:\n");
    puts(buf);

    close(fd);

    return 0;
}