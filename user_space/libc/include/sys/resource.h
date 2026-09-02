/* user_space/libc/include/sys/resource.h - M88
 *
 * `getrusage`, and the limits calls a configure script probes for.
 *
 * **What getrlimit here is, and is not.** This machine has fixed
 * ceilings - MAX_FDS is 128, a task's stack is one size, the address
 * space is what M91 gave it - and getrlimit reports them. Those are
 * real numbers and reporting them is useful.
 *
 * `setrlimit` is a truthful failure, not a no-op that returns 0. There
 * is nothing here that enforces a per-process limit, so accepting one
 * would be M65's refused permission model in a different costume: a
 * program that lowered RLIMIT_NOFILE and was told yes would believe an
 * open past that number will fail, and it will not. It returns -1 with
 * EPERM, the same shape `chmod` takes and for the same reason. The one
 * case it accepts is setting a limit to exactly what it already is,
 * which asks the system to change nothing and is therefore a request
 * this system can honestly grant - and it is the case a build script
 * that raises a soft limit to its hard limit actually hits.
 */
#pragma once

#include <stdint.h>
#include <sys/time.h> /* struct timeval */

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

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1) /* the values getrusage() is specified with; SYS_rusage's own are in proc.h */

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  5
#define RLIMIT_AS      6
#define RLIMIT_NPROC   7
/* M89: the rest of the set, so that a program looping over every limit
 * has a number for each. All of them report RLIM_INFINITY for the same
 * reason the eight above do - nothing here enforces a limit; see the
 * header note. Each names something this machine does not have at all,
 * which is a second reason and a stronger one: there is no nice value
 * (M69), no swap to lock against, no signal queue, no message queues,
 * no realtime priority, no file locks in the kernel and no pending-
 * signal count beyond one bit per signal. */
#define RLIMIT_MEMLOCK 8
#define RLIMIT_RSS     9
#define RLIMIT_NICE    10
#define RLIMIT_RTPRIO  11
#define RLIMIT_SIGPENDING 12
#define RLIMIT_MSGQUEUE 13
#define RLIMIT_LOCKS   14
/* 15, and guarded, because toybox's portability layer defines this one
 * itself for any system that does not - and a second definition with a
 * different number would be two names for two different limits that a
 * program thinks are one. Taking its value is the cheaper half of
 * agreeing. */
#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME  15
#endif
#define RLIM_NLIMITS   16

/* M89: getpriority/setpriority's `which`. Defined so that a program
 * naming one compiles; there is no scheduling priority a process can
 * set here (M69 decided that from a measurement), so both calls are
 * absent rather than present-and-lying - a program that needs them gets
 * a link error, which is the loudest possible answer. */
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

typedef uint64_t rlim_t;

/* "No limit". The value programs compare against, so it has to be the
 * one they expect: (rlim_t)-1, which is what RLIM_INFINITY is anywhere
 * rlim_t is unsigned 64-bit. */
#define RLIM_INFINITY ((rlim_t)-1)

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

/* The fields this machine measures are the two time ones. Every other
 * member is zero, and that is `getrusage`'s documented behaviour for a
 * field the implementation does not track rather than an accident -
 * nothing here counts page faults, resident pages or context switches,
 * and a plausible-looking number would be worse than a zero a caller can
 * recognise. See <sys/times.h> on the unit. */
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

int getrusage(int who, struct rusage *usage);
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

#ifdef __cplusplus
}
#endif
