#include <sys/resource.h>
#include <sys/times.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "os_resource.h"
#include "process.h"
#include "syscall_wrappers.h"

static long tick_hz(void) {
    long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
}

clock_t times(struct tms *buffer) {
    os_rusage_t self, kids;
    if (sys_rusage(OS_RUSAGE_SELF, &self) != 0 ||
        sys_rusage(OS_RUSAGE_CHILDREN, &kids) != 0) {
        errno = EINVAL;
        return (clock_t)-1;
    }
    if (buffer) {
        buffer->tms_utime = (clock_t)self.user_ticks;
        buffer->tms_stime = (clock_t)self.sys_ticks;
        buffer->tms_cutime = (clock_t)kids.user_ticks;
        buffer->tms_cstime = (clock_t)kids.sys_ticks;
    }
    return (clock_t)((unsigned long)sys_uptime_ms() * (unsigned long)tick_hz() / 1000u);
}

int getrusage(int who, struct rusage *usage) {
    if (!usage) {
        errno = EFAULT;
        return -1;
    }
    int which;
    if (who == RUSAGE_SELF) {
        which = OS_RUSAGE_SELF;
    } else if (who == RUSAGE_CHILDREN) {
        which = OS_RUSAGE_CHILDREN;
    } else if (who == RUSAGE_THREAD) {
        which = OS_RUSAGE_THREAD;
    } else {
        errno = EINVAL;
        return -1;
    }
    os_rusage_t r;
    if (sys_rusage(which, &r) != 0) {
        errno = EINVAL;
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    long hz = tick_hz();
    usage->ru_utime.tv_sec = (time_t)(r.user_ticks / (unsigned long)hz);
    usage->ru_utime.tv_usec = (long)((r.user_ticks % (unsigned long)hz) * (1000000u / (unsigned long)hz));
    usage->ru_stime.tv_sec = (time_t)(r.sys_ticks / (unsigned long)hz);
    usage->ru_stime.tv_usec = (long)((r.sys_ticks % (unsigned long)hz) * (1000000u / (unsigned long)hz));
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb < 1) {
        page_kb = 1;
    }
    usage->ru_maxrss = (long)(r.max_rss_pages * (unsigned long)page_kb);
    return 0;
}

/* getrlimit(2) and setrlimit(2) are system calls, and this library used to
   answer them itself out of a table of constants. Two things were wrong with
   that, and only the second one killed anything.

   The first is that the numbers were a claim rather than a question: a
   program linked against OPEN_MAX would go on believing it after the kernel
   enforcing it had changed. The kernel knows its own descriptor table, its
   own task table and the stack the loader laid out, so the kernel answers.

   The second is the pointer, and it is the reason this moved. A system call
   that cannot write its answer reports EFAULT; a function in this library
   writes through the caller's pointer and the caller takes SIGSEGV instead.
   That difference is load-bearing somewhere real: Chromium's
   base::ProtectedMemory makes a page of its own data read-only and then
   calls getrlimit ON THAT PAGE, requiring -1 and EFAULT as its proof that
   the page is protected - and every renderer on this machine died there.
   Only the kernel can look at a page table before it writes.

   So the caller's pointer goes to the kernel untouched. struct rlimit is
   os_rlimit_t - two 64-bit values in that order - and the checks below say
   so rather than the comment doing it. */
_Static_assert(sizeof(struct rlimit) == sizeof(os_rlimit_t),
               "struct rlimit is the ABI's os_rlimit_t");
_Static_assert(sizeof(rlim_t) == sizeof(uint64_t),
               "rlim_t is the width the kernel writes");
_Static_assert(__builtin_offsetof(struct rlimit, rlim_cur) ==
               __builtin_offsetof(os_rlimit_t, current),
               "rlim_cur is os_rlimit_t::current");
_Static_assert(__builtin_offsetof(struct rlimit, rlim_max) ==
               __builtin_offsetof(os_rlimit_t, maximum),
               "rlim_max is os_rlimit_t::maximum");
_Static_assert(RLIMIT_CPU == OS_RLIMIT_CPU, "RLIMIT_CPU is the ABI's number");
_Static_assert(RLIMIT_STACK == OS_RLIMIT_STACK, "RLIMIT_STACK is the ABI's number");
_Static_assert(RLIMIT_NOFILE == OS_RLIMIT_NOFILE, "RLIMIT_NOFILE is the ABI's number");
_Static_assert(RLIMIT_NPROC == OS_RLIMIT_NPROC, "RLIMIT_NPROC is the ABI's number");
_Static_assert(RLIMIT_NICE == OS_RLIMIT_NICE, "RLIMIT_NICE is the ABI's number");
_Static_assert(RLIM_NLIMITS == OS_RLIM_COUNT, "the two tables are the same length");
_Static_assert(RLIM_INFINITY == (rlim_t)OS_RLIM_INFINITY,
               "infinity is the same value on both sides");

static int rlimit_errno(long r) {
    if (r == -OS_ERROR_FAULT) {
        return EFAULT;
    }
    if (r == -OS_ERROR_PERMISSION) {
        return EPERM;
    }
    return EINVAL;
}

int getrlimit(int resource, struct rlimit *rlim) {
    long r = sys_getrlimit(resource, rlim);
    if (r != 0) {
        errno = rlimit_errno(r);
        return -1;
    }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim) {
    long r = sys_setrlimit(resource, rlim);
    if (r != 0) {
        errno = rlimit_errno(r);
        return -1;
    }
    return 0;
}

/* Every process on this machine has a nice value of zero, and that is a
   property of the scheduler rather than a default: kernel/scheduler has two
   priority CLASSES and no per-task priority number at all. So getpriority
   reports the one value there is, and setpriority takes it and refuses any
   other with EPERM. M65's rule is what decides the second half - a
   setpriority that returned 0 and changed nothing would be a call that
   pretends to enforce something. */
int getpriority(int which, id_t who) {
    (void)who;
    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int setpriority(int which, id_t who, int value) {
    (void)who;
    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER) {
        errno = EINVAL;
        return -1;
    }
    if (value == 0) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

