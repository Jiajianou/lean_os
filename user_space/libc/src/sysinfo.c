/* user_space/libc/src/sysinfo.c - M89
 *
 * `uname`, `sched_yield` and the scattered-I/O pair. Three small things
 * that share a file because each is a handful of lines and none of them
 * has a natural neighbour.
 */
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/utsname.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "proc.h" /* system_api/include/proc.h - os_meminfo_t and task_info_t, M89 */
#include "syscall_wrappers.h"

/* Copies a NUL-terminated string into a fixed field. */
static void field(char *dst, const char *src) {
    size_t i = 0;
    for (; src[i] && i < _UTSNAME_LENGTH - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int uname(struct utsname *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    field(buf->sysname, "lean_os");
    /* This machine has no name. It was never given one, there is no
     * hostname call in this kernel to have set one, and a program that
     * reads nodename to label output gets an empty string rather than a
     * plausible "localhost" - which would be a name somebody could
     * mistake for configuration. See <sys/utsname.h>. */
    field(buf->nodename, "");
    /* The milestone this kernel was built at. A version string is a
     * claim about what a program can expect, and the honest one here is
     * the number this file's own history is kept under. */
    field(buf->release, "0.89");
    field(buf->version, "lean_os M89");
    field(buf->machine, "x86_64");
    field(buf->domainname, "");
    return 0;
}

int sched_yield(void) {
    sys_yield();
    return 0; /* SYS_yield cannot fail: it returns when this task runs again */
}

/* The priority calls, which are refusals - see <sched.h>. This
 * scheduler's classes are scheduler-owned (M69: "a flag a program sets
 * is a flag every program sets"), so agreeing would be telling a program
 * it had been given a priority it does not have. */
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    (void)pid;
    (void)policy;
    (void)param;
    errno = ENOSYS;
    return -1;
}

int sched_getscheduler(pid_t pid) {
    (void)pid;
    /* This one can answer truthfully: every task here is scheduled by
     * the same round-robin-with-two-classes policy, and SCHED_OTHER is
     * what that is called everywhere. */
    return SCHED_OTHER;
}

int sched_get_priority_min(int policy) {
    (void)policy;
    return 0;
}

int sched_get_priority_max(int policy) {
    (void)policy;
    /* Min and max are both 0, which is how POSIX says a policy with no
     * priority range reports itself. A program that computes a midpoint
     * gets 0 and sets nothing, rather than a number this system would
     * have to refuse. */
    return 0;
}

/* ---- scattered I/O ---------------------------------------------------
 *
 * A loop, and <sys/uio.h> says what that costs: no atomicity. Returning
 * the byte count of a partial vector is deliberate and is what the real
 * calls do - a short readv is not an error. */
ssize_t readv(int fd, const struct iovec *iov, int count) {
    if (!iov || count < 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            /* Bytes already transferred are reported rather than lost;
             * only a failure on the first element is an error, which is
             * the contract every caller unwinds against. */
            return total > 0 ? total : -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break; /* short read - end of file or all that was available */
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int count) {
    if (!iov || count < 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return total > 0 ? total : -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

/* ---- the calls this machine does not have ---------------------------
 *
 * Each returns -1 with ENOSYS rather than 0. See <sys/mount.h> for why
 * each is missing. A stub returning success here would be the most
 * expensive kind of lie: mount would have a program believe a filesystem
 * is present.
 *
 * M85 (second attempt): openpty, forkpty and login_tty used to be here
 * and are not any more. They are real now - user_space/libc/src/pty.c
 * over /dev/ptmx - which is the outcome this comment was written
 * expecting.
 */
#include <sys/mount.h>

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    (void)source;
    (void)target;
    (void)fstype;
    (void)flags;
    (void)data;
    errno = ENOSYS;
    return -1;
}

int umount(const char *target) {
    (void)target;
    errno = ENOSYS;
    return -1;
}

int umount2(const char *target, int flags) {
    (void)target;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

/* M89: see <sys/syscall.h> for why this refuses rather than dispatching.
 * A caller that got here has a Linux syscall number in its hand, and
 * this kernel's numbers are not Linux's. */
#include <sys/syscall.h>

long syscall(long number, ...) {
    (void)number;
    errno = ENOSYS;
    return -1;
}

/* ---- M89: sysinfo - see <sys/sysinfo.h> for what each 0 means ------- */

int sysinfo(struct sysinfo *info) {
    if (!info) {
        errno = EFAULT;
        return -1;
    }
    memset(info, 0, sizeof(*info));

    info->uptime = (long)(sys_uptime_ms() / 1000);

    os_meminfo_t mi;
    if (sys_meminfo(&mi) == 0) {
        info->mem_unit = (unsigned int)mi.page_size;
        info->totalram = (unsigned long)mi.total_frames;
        info->freeram = (unsigned long)mi.free_frames;
    } else {
        info->mem_unit = 4096;
    }

    /* The process count needs CAP_PROC_LIST, which an ordinary program
     * does not have - so this is 0 for most callers rather than -1 for
     * the whole call. A refusal to report one field is not a reason to
     * refuse the other eleven, and `procs` is the one a program reading
     * this is least likely to be asking for. */
    long n = sys_taskinfo((task_info_t *)0, 0);
    if (n > 0) {
        info->procs = (unsigned short)n;
    }
    return 0;
}
