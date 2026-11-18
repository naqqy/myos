#ifndef NAQ_LIBC_TIME_H
#define NAQ_LIBC_TIME_H

#include <stddef.h>

typedef long time_t;
typedef int clockid_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char* tm_zone;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/*
 * The clock comes from the PC's battery-backed clock, read once and then
 * advanced with the system timer. There are no time zones: local time and
 * "UTC" are the same thing here.
 */
time_t time(time_t* out);
int clock_gettime(clockid_t id, struct timespec* ts);
struct tm* localtime_r(const time_t* t, struct tm* out);
struct tm* gmtime_r(const time_t* t, struct tm* out);

#endif
