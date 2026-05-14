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
    naq_stat_t st;
    char header[16];

    puts("Asset pipeline test\n");

    if (stat("/assets/game/player.ni", &st) < 0) {
        puts("stat /assets/game/player.ni failed\n");
        return 1;
    }

    puts("player.ni size=");
    print_u64(st.size);
    puts("\n");

    int fd = open("/assets/game/player.ni", 0);

    if (fd < 0) {
        puts("open failed\n");
        return 1;
    }

    i64 n = read(fd, header, sizeof(header));

    if (n < 0) {
        puts("read failed\n");
        close(fd);
        return 1;
    }

    close(fd);

    puts("read header bytes=");
    print_u64((u64)n);
    puts("\n");

    puts("asset pipeline OK\n");
    return 0;
}