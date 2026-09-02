/* user_space/libc/src/utime.c - M88
 *
 * `utime`, `utimes` and `utimensat`, over SYS_utime.
 *
 * Three spellings of one operation, which is what fifty years of Unix
 * leaves behind. All three reduce to a single seconds-since-1970 value,
 * because that is what leanfs stores - the microseconds `utimes` carries
 * and the nanoseconds `utimensat` carries are truncated, and a caller
 * that reads the time back gets whole seconds. Stated here rather than
 * discovered.
 */
#include <utime.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h> /* UTIME_NOW, UTIME_OMIT - POSIX puts them here */
#include <sys/time.h>
#include <time.h>

#include "syscall_wrappers.h"

static int set_mtime(const char *path, time_t mtime) {
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (sys_utime(path, (unsigned int)mtime) != 0) {
        /* The syscall refuses a missing path, a synthetic mount and a
         * caller without CAP_FS_WRITE, and reports one -1 for all three.
         * EACCES is the one a program can act on and the one a capability
         * refusal actually is. */
        errno = EACCES;
        return -1;
    }
    return 0;
}

int utime(const char *path, const struct utimbuf *times) {
    /* NULL means now, which is what `touch` on an existing file does.
     * The current time is read here rather than passed as a sentinel to
     * the kernel: the kernel already stamps rtc_now() on every other
     * write, so a second meaning for one value would be an ABI with a
     * magic number in it. */
    time_t when = times ? times->modtime : time((time_t *)0);
    return set_mtime(path, when);
}

/* BSD's spelling, with microseconds this filesystem does not store. */
int utimes(const char *path, const struct timeval tv[2]) {
    return set_mtime(path, tv ? tv[1].tv_sec : time((time_t *)0));
}

/* POSIX.1-2008's spelling. `dirfd` must be AT_FDCWD: the *at() family
 * does not exist on this machine yet - it is M89's, where toybox asks
 * for it by name - and a relative path resolved against the wrong
 * directory is a silent wrong answer rather than a failure. UTIME_NOW
 * and UTIME_OMIT are honoured, because a caller that passes UTIME_OMIT
 * for mtime is asking this call to leave the one field it can set alone,
 * which is a success that does nothing. */
int utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags) {
    (void)flags; /* AT_SYMLINK_NOFOLLOW: leanfs stores no time on a link */
    if (dirfd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    if (!ts) {
        return set_mtime(path, time((time_t *)0));
    }
    if (ts[1].tv_nsec == UTIME_OMIT) {
        return 0;
    }
    if (ts[1].tv_nsec == UTIME_NOW) {
        return set_mtime(path, time((time_t *)0));
    }
    return set_mtime(path, ts[1].tv_sec);
}
