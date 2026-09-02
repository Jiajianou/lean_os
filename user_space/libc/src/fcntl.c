/* user_space/libc/src/fcntl.c - M89
 *
 * The *at() family: a relative name resolved against a directory named
 * by a descriptor. M87 scheduled these, M93 absorbed them without
 * landing them, and toybox's dirtree walker asks for four of them by
 * name - which is the reason they are here and not in either of those.
 *
 * **All of it is path arithmetic, and that is the design.** The kernel
 * gained one call for this milestone (SYS_fdpath) rather than a second
 * copy of every path syscall taking a dirfd. Joining a directory name to
 * a relative one is string work, string work belongs in the C library,
 * and the alternative was eight kernel entry points that each had to
 * re-derive the same absolute path before doing what the existing one
 * already does.
 *
 * What that costs, stated once here rather than at each function: the
 * join is not atomic against a rename of the directory between the
 * SYS_fdpath and the operation. <fcntl.h> says why that is the right
 * trade on this machine and what would have to change for it not to be.
 */
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>   /* rename() lives here, which renameat is built on */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "syscall_wrappers.h"

/* Builds the absolute path `dirfd`/`path` into `out`.
 *
 * Returns 0, or -1 with errno set. An absolute `path` ignores `dirfd`
 * entirely, which is POSIX's rule and the reason every caller can pass
 * a dirfd it never checked.
 */
static int resolve_at(int dirfd, const char *path, char *out, size_t out_len) {
    if (!path || !out) {
        errno = EFAULT;
        return -1;
    }
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        size_t n = strlen(path);
        if (n + 1 > out_len) {
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
    long dn = sys_fdpath(dirfd, out, (unsigned long)out_len);
    if (dn < 0) {
        /* Not an open file, or a path too long to have been recorded.
         * EBADF is right for the first and the closest true answer for
         * the second: this descriptor cannot be used to name anything. */
        errno = EBADF;
        return -1;
    }
    size_t n = (size_t)dn;
    /* An empty relative name means the directory itself - AT_EMPTY_PATH's
     * case, and also what dirtree passes for the root of a walk. */
    if (path[0] == '\0') {
        return 0;
    }
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= out_len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[n++] = '/';
    }
    size_t pn = strlen(path);
    if (n + pn + 1 > out_len) {
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
    /* AT_REMOVEDIR is what makes one call do both jobs, and it is the
     * flag a caller gets wrong: without it, unlinkat on a directory must
     * fail rather than remove it. */
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
    /* Note the target is NOT resolved: a symlink's contents are a string
     * this filesystem stores verbatim, and resolving it here would turn
     * a relative link into an absolute one behind the caller's back. */
    return symlink(target, full);
}

int linkat(int oldfd, const char *oldpath, int newfd, const char *newpath,
           int flags) {
    char a[PATH_MAX];
    char b[PATH_MAX];
    (void)flags; /* AT_SYMLINK_FOLLOW: link() here already follows */
    if (resolve_at(oldfd, oldpath, a, sizeof(a)) != 0 ||
        resolve_at(newfd, newpath, b, sizeof(b)) != 0) {
        return -1;
    }
    return link(a, b);
}

long readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return readlink(full, buf, bufsiz);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    char full[PATH_MAX];
    (void)flags; /* AT_EACCESS - see <fcntl.h> */
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return access(full, mode);
}

/* fstatat lives here rather than in stat.c because it is the same join
 * as everything above and the join is what it is made of. Its declaration
 * is in <sys/stat.h>, where a program looks for it. */
int fstatat(int dirfd, const char *path, struct stat *out, int flags) {
    char full[PATH_MAX];
    if (resolve_at(dirfd, path, full, sizeof(full)) != 0) {
        return -1;
    }
    return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(full, out) : stat(full, out);
}
