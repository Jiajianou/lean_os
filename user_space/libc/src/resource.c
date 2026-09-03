/* user_space/libc/src/resource.c - M88
 *
 * `times`, `getrusage`, `getrlimit` and `setrlimit`, over SYS_rusage.
 *
 * One syscall behind four functions, because the two that report CPU
 * time are the same question asked in two shapes and the two that report
 * limits are answered from constants this libc already knows. See
 * <sys/resource.h> for why setrlimit refuses rather than agreeing.
 */
#include <sys/resource.h>
#include <sys/times.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "proc.h" /* system_api/include/proc.h - os_rusage_t, OS_RUSAGE_* */
#include "syscall_wrappers.h"

/* The tick rate SYS_rusage counts in, which is also what sysconf reports
 * as _SC_CLK_TCK. Asked of sysconf rather than written as 100 here, so
 * there is one place this machine's tick rate is stated and a change to
 * PIT_HZ cannot leave two answers behind. */
static long tick_hz(void) {
    long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
}

clock_t times(struct tms *buf) {
    os_rusage_t self, kids;
    if (sys_rusage(OS_RUSAGE_SELF, &self) != 0 ||
        sys_rusage(OS_RUSAGE_CHILDREN, &kids) != 0) {
        errno = EINVAL;
        return (clock_t)-1;
    }
    if (buf) {
        buf->tms_utime = (clock_t)self.user_ticks;
        buf->tms_stime = (clock_t)self.sys_ticks;
        buf->tms_cutime = (clock_t)kids.user_ticks;
        buf->tms_cstime = (clock_t)kids.sys_ticks;
    }
    /* The return value is elapsed real time in the same units - what a
     * caller subtracts two of to time an interval. Uptime is the clock
     * that answers that; it is milliseconds, so it is scaled down to
     * ticks rather than reported at a resolution times() cannot mean. */
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
    } else {
        errno = EINVAL; /* RUSAGE_THREAD has no separate meaning here - see <sys/resource.h> */
        return -1;
    }
    os_rusage_t r;
    if (sys_rusage(which, &r) != 0) {
        errno = EINVAL;
        return -1;
    }
    /* Everything this machine does not count is zero, which is what
     * getrusage specifies for an unsupported field - and the memset is
     * how that stays true when somebody adds a member to the struct. */
    memset(usage, 0, sizeof(*usage));
    long hz = tick_hz();
    usage->ru_utime.tv_sec = (time_t)(r.user_ticks / (unsigned long)hz);
    usage->ru_utime.tv_usec = (long)((r.user_ticks % (unsigned long)hz) * (1000000u / (unsigned long)hz));
    usage->ru_stime.tv_sec = (time_t)(r.sys_ticks / (unsigned long)hz);
    usage->ru_stime.tv_usec = (long)((r.sys_ticks % (unsigned long)hz) * (1000000u / (unsigned long)hz));
    /* M98: and the one memory field this machine now measures. Kilobytes,
     * because that is what ru_maxrss means on every system a program was
     * written against - the kernel reports pages, and the page size comes
     * from the same place sysconf's _SC_PAGESIZE does rather than from a
     * 4096 written here. */
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb < 1) {
        page_kb = 1;
    }
    usage->ru_maxrss = (long)(r.max_rss_pages * (unsigned long)page_kb);
    return 0;
}

/* The ceilings this machine actually has. Each is a real number rather
 * than RLIM_INFINITY-by-default: an open past _SC_OPEN_MAX genuinely
 * fails, and a program that was told "no limit" would find that out the
 * hard way. Where there is honestly no limit - a file's size is bounded
 * by the disk, not by a per-process rule - the answer is infinity and
 * that is equally a fact. */
static int limit_for(int resource, struct rlimit *out) {
    switch (resource) {
    case RLIMIT_NOFILE:
        out->rlim_cur = out->rlim_max = (rlim_t)sysconf(_SC_OPEN_MAX);
        return 0;
    case RLIMIT_STACK:
        /* USER_STACK_SIZE - one 8 MiB region per process, fixed at
         * spawn and not growable by asking. */
        out->rlim_cur = out->rlim_max = (rlim_t)(8u * 1024u * 1024u);
        return 0;
    case RLIMIT_NPROC:
        out->rlim_cur = out->rlim_max = (rlim_t)TASK_INFO_MAX;
        return 0;
    case RLIMIT_CORE:
        /* No core dumps exist here at all, and 0 is exactly how a Unix
         * system says so. */
        out->rlim_cur = out->rlim_max = 0;
        return 0;
    case RLIMIT_CPU:
    case RLIMIT_FSIZE:
    case RLIMIT_DATA:
    case RLIMIT_AS:
        out->rlim_cur = out->rlim_max = RLIM_INFINITY;
        return 0;
    default:
        return -1;
    }
}

int getrlimit(int resource, struct rlimit *rlim) {
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }
    if (limit_for(resource, rlim) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim) {
    struct rlimit current;
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }
    if (limit_for(resource, &current) != 0) {
        errno = EINVAL;
        return -1;
    }
    /* Setting a limit to what it already is asks this system to change
     * nothing, which it can honestly do - and it is the case a build
     * script raising a soft limit to its hard limit actually hits.
     * Anything else is refused: see <sys/resource.h> on why agreeing
     * would be M65's refused permission model wearing a different hat. */
    if (rlim->rlim_cur == current.rlim_cur && rlim->rlim_max == current.rlim_max) {
        return 0;
    }
    errno = EPERM;
    return -1;
}
