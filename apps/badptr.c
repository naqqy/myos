#include "syscall.h"

void app_main(void) {
    app_write("About to test bad pointer...\n");

    syscall2(SYS_WRITE, 0xDEADBEEFu, 16);

    app_write("Kernel survived bad pointer syscall.\n");
}