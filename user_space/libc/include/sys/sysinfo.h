/* user_space/libc/include/sys/sysinfo.h - M89
 *
 * One call that reports several facts about the machine at once.
 *
 * Every field below is either a real number this kernel keeps or a zero
 * that means "this machine has no such thing", and the two are named
 * field by field rather than summarised - a program reading `totalswap`
 * and getting 0 should be able to find out here whether that means no
 * swap or no answer. It means no swap: there is none (see M82 and M90's
 * deferrals), and 0 is the correct total.
 *
 * `loads` is the one field with nothing behind it. A load average is a
 * decaying count of runnable tasks that this scheduler does not keep -
 * sched.c counts tasks and ticks, not an exponentially-weighted average
 * - so the three entries are 0. That is a missing number reported as a
 * zero, which is the one place in this header a caller could be misled,
 * and it is why it is stated here.
 */
#pragma once

#include <stdint.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

struct sysinfo {
    long uptime;             /* seconds since boot - SYS_uptime_ms */
    unsigned long loads[3];  /* 0, 0, 0 - see the header note */
    unsigned long totalram;  /* frames * mem_unit */
    unsigned long freeram;
    unsigned long sharedram; /* 0 - shared memory is not accounted separately */
    unsigned long bufferram; /* 0 - M92's block cache is kernel heap, not a pool */
    unsigned long totalswap; /* 0 - there is no swap on this machine */
    unsigned long freeswap;  /* 0 */
    unsigned short procs;    /* live tasks, when this process may count them */
    unsigned long totalhigh; /* 0 - no high memory on x86-64 */
    unsigned long freehigh;  /* 0 */
    unsigned int mem_unit;   /* the page size: totalram is in these */
};

int sysinfo(struct sysinfo *info);

#ifdef __cplusplus
}
#endif
