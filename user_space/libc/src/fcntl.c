#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "syscall_wrappers.h"

static int resolve_at(int dirfd, const char *path, char *out, size_t out_length) {
    if (!path || !out) {
        errno = EFAULT;
        return -1;
    }
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        size_t n = strlen(path);
        if (n + 1 > out_length) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out, path, n + 1);
        return 0;
    }
    if (dirfd < 0) {
        errno = EBADF;
        return -1;
    }
    long dn = sys_fdpath(dirfd, out, (unsigned long)out_length);
    if (dn < 0) {
        errno = EBADF;
        return -1;
    }
    size_t n = (size_t)dn;
    if (path[0] == '\0') {
        return 0;
    }
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= out_length) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[n++] = '/';
    }
    size_t pn = strlen(path);
    if (n + pn + 1 > out_length) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out + n, path, pn + 1);
    return 0;
}

int openat(int dirfd, const char *path, int flags, ...) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return open(full, flags);
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return mkdir(full, mode);
}

int unlinkat(int dirfd, const char *path, int flags) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return (flags & AT_REMOVEDIR) ? rmdir(full) : unlink(full);
}

int renameat(int oldfd, const char *oldpath, int newfd, const char *newpath) {
    char a[PATH_MAX];
    char b[PATH_MAX];
    if (resolve_at(oldfd, oldpath, a, sizeof(a)) != 0 ||
        resolve_at(newfd, newpath, b, sizeof(b)) != 0) {
        return -1;
    }
    return rename(a, b);
}

int symlinkat(const char *target, int dirfd, const char *path) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return symlink(target, full);
}

int linkat(int oldfd, const char *oldpath, int newfd, const char *newpath,
           int flags) {
    char a[PATH_MAX];
    char b[PATH_MAX];
    (void)flags;
    if (resolve_at(oldfd, oldpath, a, sizeof(a)) != 0 ||
        resolve_at(newfd, newpath, b, sizeof(b)) != 0) {
        return -1;
    }
    return link(a, b);
}

long readlinkat(int dirfd, const char *path, char *buffer, size_t bufsiz) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return readlink(full, buffer, bufsiz);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    char full[PATH_MAX];
    (void)flags;
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return access(full, mode);
}

int fstatat(int dirfd, const char *path, struct stat *out, int flags) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(full, out) : stat(full, out);
}
