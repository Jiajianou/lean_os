#include <sys/resource.h>
#include <sys/times.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "proc.h"
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

static int limit_for(int resource, struct rlimit *out) {
    switch (resource) {
    case RLIMIT_NOFILE:
        out->rlim_cur = out->rlim_max = (rlim_t)sysconf(_SC_OPEN_MAX);
        return 0;
    case RLIMIT_STACK:
        out->rlim_cur = out->rlim_max = (rlim_t)(8u * 1024u * 1024u);
        return 0;
    case RLIMIT_NPROC:
        out->rlim_cur = out->rlim_max = (rlim_t)TASK_INFO_MAX;
        return 0;
    case RLIMIT_CORE:
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
    if (rlim->rlim_cur == current.rlim_cur && rlim->rlim_max == current.rlim_max) {
        return 0;
    }
    errno = EPERM;
    return -1;
}
