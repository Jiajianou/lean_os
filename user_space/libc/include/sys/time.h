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

/* M89: set the wall clock, over SYS_settime. Real - this is what
 * `nettime` already does through the lean_os call, and `date -s` is the
 * same operation asked for by a program somebody else wrote.
 *
 * The microseconds are dropped: SYS_settime takes whole seconds, and a
 * microsecond field this could not honour would be a number a caller
 * sets and cannot read back. `tz` is ignored, as POSIX says it should be
 * - there is one timezone here and it is UTC (see <time.h>).
 *
 * Note what SYS_settime does and does not touch: it sets the kernel's
 * own offset, not the CMOS RTC. So a `date -s` here survives until the
 * machine is switched off and not past it, which is the honest
 * behaviour of the call underneath and is stated so nobody looks for a
 * battery-backed clock that was never written. */
int settimeofday(const struct timeval *tv, const void *tz);

/* M88: BSD's spelling of utime(), carrying microseconds this filesystem
 * does not store. tv[0] is the access time, which leanfs does not keep
 * either; tv[1] is the modification time and is the one that lands. */
int utimes(const char *path, const struct timeval tv[2]);
