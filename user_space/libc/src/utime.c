#include <utime.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>

#include "syscall_wrappers.h"

static int set_mtime(const char *path, time_t mtime) {
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (sys_utime(path, (unsigned int)mtime) != 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int utime(const char *path, const struct utimbuf *times) {
    time_t when = times ? times->modtime : time((time_t *)0);
    return set_mtime(path, when);
}

int utimes(const char *path, const struct timeval tv[2]) {
    return set_mtime(path, tv ? tv[1].tv_sec : time((time_t *)0));
}

/* The descriptor's own path, then the same one operation. There is no
   syscall that sets a time through a descriptor, and adding one for this
   would be a second way to do what sys_utime already does. */
int futimes(int fd, const struct timeval tv[2]) {
    char path[PATH_MAX];
    long n = sys_fdpath(fd, path, sizeof(path));
    if (n < 0) {
        errno = EBADF;
        return -1;
    }
    path[n] = '\0';
    return utimes(path, tv);
}

int utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags) {
    (void)flags;
    char full[PATH_MAX];
    if (dirfd != AT_FDCWD && path && path[0] != '/') {
        long n = sys_fdpath(dirfd, full, sizeof(full));
        if (n < 0) {
            errno = EBADF;
            return -1;
        }
        size_t at = (size_t)n;
        if (at > 0 && full[at - 1] != '/') {
            full[at++] = '/';
        }
        size_t i = 0;
        while (path[i] && at + 1 < sizeof(full)) {
            full[at++] = path[i++];
        }
        if (path[i]) {
            errno = ENAMETOOLONG;
            return -1;
        }
        full[at] = '\0';
        path = full;
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

int futimens(int fd, const struct timespec ts[2]) {
    char path[PATH_MAX];
    if (sys_fdpath(fd, path, sizeof(path)) < 0) {
        errno = EBADF;
        return -1;
    }
    return utimensat(AT_FDCWD, path, ts, 0);
}
