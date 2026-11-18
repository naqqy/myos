#include "../include/naqnet.h"

/*
 * TCP helpers that wait, built on the non-waiting socket system calls.
 */

int tcp_connect(const char* host, u16 port, u32 timeout_ms) {
    u32 ip = 0;

    if (!net_ready() || net_resolve(host, &ip) != 0) {
        return -1;
    }

    int handle = sock_open(ip, port);

    if (handle <= 0) {
        return -1;
    }

    u64 start = get_ticks();

    for (;;) {
        int state = sock_state(handle);

        if (state == SOCK_OPEN) {
            return handle;
        }

        if (state != SOCK_CONNECTING ||
            time_ticks_to_ms(get_ticks() - start) >= timeout_ms) {
            sock_close(handle);
            return -1;
        }

        sleep_ms(2);
    }
}

int tcp_write_all(int handle, const void* data, u32 length, u32 timeout_ms) {
    const u8* p = (const u8*)data;
    u64 start = get_ticks();

    while (length > 0) {
        int taken = sock_send(handle, p, length);

        if (taken < 0) {
            return 0;
        }

        if (taken == 0) {
            if (time_ticks_to_ms(get_ticks() - start) >= timeout_ms) {
                return 0;
            }

            sleep_ms(2);
            continue;
        }

        p += taken;
        length -= (u32)taken;
        start = get_ticks();
    }

    return 1;
}

int tcp_read(int handle, void* buffer, u32 capacity, u32 timeout_ms) {
    u64 start = get_ticks();

    for (;;) {
        int got = sock_recv(handle, buffer, capacity);

        if (got > 0) {
            return got;
        }

        if (got == -1) {
            return 0;
        }

        if (got < 0) {
            return -2;
        }

        if (time_ticks_to_ms(get_ticks() - start) >= timeout_ms) {
            return -1;
        }

        sleep_ms(1);
    }
}
