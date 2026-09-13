#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

static void fill(struct stat *out, const os_stat_t *st) {
    memset(out, 0, sizeof(*out));
    if (st->is_link) {
        out->st_mode = S_IFLNK;
    } else {
        switch (st->kind) {
        case OS_STAT_DIR:  out->st_mode = S_IFDIR; break;
        case OS_STAT_CHR:  out->st_mode = S_IFCHR; break;
        case OS_STAT_FIFO: out->st_mode = S_IFIFO; break;
        case OS_STAT_SOCK: out->st_mode = S_IFSOCK; break;
        default:           out->st_mode = S_IFREG; break;
        }
    }
    out->st_size = (off_t)st->size;
    out->st_mtime = (time_t)st->mtime;
    out->st_atime = out->st_mtime;
    out->st_ctime = out->st_mtime;
    out->st_mtim.tv_nsec = 0;
    out->st_atim.tv_nsec = 0;
    out->st_ctim.tv_nsec = 0;
    out->st_rdev = 0;
    out->st_ino = (ino_t)st->inode;
    out->st_nlink = 1;
    out->st_blksize = 4096;
    out->st_blocks = (blkcnt_t)((st->size + 511u) / 512u);
}

static long stat_errno(long r) {
    if (r == -OS_ERR_NOENT) {
        errno = ENOENT;
    } else if (r == -OS_ERR_FAULT) {
        errno = EFAULT;
    }
    return r;
}

int stat(const char *path, struct stat *out) {
    os_stat_t st;
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    if (stat_errno(sys_stat(path, &st)) != 0) {
        return -1;
    }
    fill(out, &st);
    return 0;
}

int lstat(const char *path, struct stat *out) {
    os_stat_t st;
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    if (stat_errno(sys_lstat(path, &st)) != 0) {
        return -1;
    }
    fill(out, &st);
    return 0;
}

int fstat(int fd, struct stat *out) {
    os_stat_t st;
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    if (sys_fstat(fd, &st) != 0) {
        errno = EBADF;
        return -1;
    }
    fill(out, &st);
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    if (sys_mkdir(path) != 0) {
        errno = __lean_path_errno(path, 1);
        return -1;
    }
    return 0;
}

static int mode_is_what_stat_reports(mode_t mode) {
    return (mode & 07777) == 0;
}

int chmod(const char *path, mode_t mode) {
    (void)path;
    if (mode_is_what_stat_reports(mode)) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int fchmod(int fd, mode_t mode) {
    (void)fd;
    if (mode_is_what_stat_reports(mode)) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    (void)dirfd;
    (void)path;
    (void)flags;
    if (mode_is_what_stat_reports(mode)) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int mkfifo(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = EOPNOTSUPP;
    return -1;
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    (void)path;
    (void)mode;
    (void)dev;
    errno = EOPNOTSUPP;
    return -1;
}

static mode_t umask_value = 022;

mode_t umask(mode_t mask) {
    mode_t old = umask_value;
    umask_value = mask & 07777;
    return old;
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) {
    (void)dirfd;
    return mknod(path, mode, dev);
}

int mkfifoat(int dirfd, const char *path, mode_t mode) {
    (void)dirfd;
    return mkfifo(path, mode);
}
