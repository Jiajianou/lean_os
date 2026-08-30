/* user_space/libc/include/sys/time.h - M80 groundwork
 *
 * `struct timeval` and `gettimeofday`, over SYS_time and SYS_uptime_ms.
 *
 * The microseconds field is real to the millisecond and zero below that,
 * because that is the resolution of the only clock this OS exposes to
 * user space (drivers/pit.h ticks at 100 Hz and SYS_uptime_ms reports
 * milliseconds). Reporting a microsecond figure derived from nothing
 * would be a precision this machine does not have.
 */
#pragma once

#include <time.h>

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/* `tz` is accepted and ignored - it has been meaningless on every system
 * since 4.3BSD, and this one has no timezone database at all. */
int gettimeofday(struct timeval *tv, void *tz);
