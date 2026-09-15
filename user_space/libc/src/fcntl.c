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

int posix_fadvise(int fd, off_t offset, off_t length, int advice) {
    (void)offset;
    (void)length;
    if (fd < 0) {
        return EBADF;
    }
    switch (advice) {
    case POSIX_FADV_NORMAL:
    case POSIX_FADV_RANDOM:
    case POSIX_FADV_SEQUENTIAL:
    case POSIX_FADV_WILLNEED:
    case POSIX_FADV_DONTNEED:
    case POSIX_FADV_NOREUSE:
        return 0;
    default:
        return EINVAL;
    }
}

/* Making sure the space is there is one operation this filesystem has:
   extending the file. It is not the same guarantee Linux gives - leanfs
   allocates blocks when they are written rather than when the size moves, so
   a later write can still fail for want of a block - and the comment is here
   rather than a claim that it cannot. */
int fallocate(int fd, int mode, off_t offset, off_t length) {
    if (length <= 0 || offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (mode & FALLOC_FL_PUNCH_HOLE) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (mode & ~FALLOC_FL_KEEP_SIZE) {
        errno = EINVAL;
        return -1;
    }
    if (mode & FALLOC_FL_KEEP_SIZE) {
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    off_t want = offset + length;
    if (st.st_size >= want) {
        return 0;
    }
    return ftruncate(fd, want);
}

int posix_fallocate(int fd, off_t offset, off_t length) {
    if (fallocate(fd, 0, offset, length) != 0) {
        return errno;
    }
    return 0;
}
