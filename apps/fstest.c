#include "syscall.h"

static char buf[4096];

void app_main(void) {
    int n;

    app_write("FSTEST starting...\n");

    if (app_mkdir("/apps_test") == 0) {
        app_write("mkdir /apps_test ok\n");
    } else {
        app_write("mkdir /apps_test failed or already exists\n");
    }

    n = app_read_file("/docs/readme.txt", buf, sizeof(buf) - 1);

    if (n < 0) {
        app_write("read /docs/readme.txt failed\n");
    } else {
        buf[n] = 0;
        app_write("read /docs/readme.txt ok:\n");
        app_write(buf);
        app_write("\n");
    }

    if (app_rmdir("/apps_test") == 0) {
        app_write("rmdir /apps_test ok\n");
    } else {
        app_write("rmdir /apps_test failed\n");
    }
}