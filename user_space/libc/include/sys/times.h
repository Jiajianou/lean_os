/* user_space/libc/include/sys/times.h - M88
 *
 * `times()`, which is how a program asks where its own CPU time went and
 * how much its children have used. `make` calls it to report build
 * times, and a configure script probes for it early.
 *
 * The unit is a clock tick at sysconf(_SC_CLK_TCK), which on this
 * machine is the PIT's own 100 Hz. That is not a convention borrowed
 * from somewhere - it is the only rate anything here measures at, since
 * SYS_rusage's counters are incremented by the timer interrupt itself.
 * A libc that reported microseconds would be multiplying a number that
 * was counted in centiseconds and presenting the result as precision it
 * does not have.
 */
#pragma once

#include <time.h> /* clock_t, and CLOCKS_PER_SEC's neighbour */

struct tms {
    clock_t tms_utime;  /* this process, in user code */
    clock_t tms_stime;  /* this process, in the kernel on its behalf */
    clock_t tms_cutime; /* its REAPED children, in user code */
    clock_t tms_cstime; /* its reaped children, in the kernel */
};

/* Returns elapsed real time since an arbitrary point in the past, in the
 * same ticks - which is what a caller subtracts two of to get a wall
 * clock for the interval it just measured - or (clock_t)-1.
 *
 * The children's fields count only what has been waited for. That is
 * what POSIX specifies and it is also the only version that can be
 * right: a child still running has time that is still accruing. */
clock_t times(struct tms *buf);
