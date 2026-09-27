#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

/* The permission bits, which this filesystem does not store because there
   is nothing for them to decide: one principal, and the kernel refuses that
   principal nothing a file supports. So every object reports what is true of
   it - its owner may read, write and execute it, and there is no group and
   no other to grant anything to - which is 0700. A symbolic link reports
   0777, as Linux's do, because a link's own bits are never consulted.

   Until M187 this was zero, meant as "no permission model" - but POSIX reads
   0000 as "nobody may do anything", which is the opposite of the truth, and
   programs ask the question precisely: Chromium's ProcessSingleton requires
   its socket directory to be exactly 0700, i.e. private to its owner, and
   died on its first second here; Rust's Permissions::readonly() is "no write
   bits", so every file on this machine read as read-only there. */
#define OWNER_MAY_DO_ANYTHING 0700
#define LINK_MODE             0777

static void fill(struct stat *out, const os_stat_t *st) {
    memset(out, 0, sizeof(*out));
    if (st->is_link) {
        out->st_mode = S_IFLNK;
    } else {
        switch (st->kind) {
        case OS_STAT_DIRECTORY:  out->st_mode = S_IFDIR; break;
        case OS_STAT_CHR:  out->st_mode = S_IFCHR; break;
        case OS_STAT_FIFO: out->st_mode = S_IFIFO; break;
        case OS_STAT_SOCKET: out->st_mode = S_IFSOCK; break;
        default:           out->st_mode = S_IFREG; break;
        }
    }
    out->st_mode |= st->is_link ? LINK_MODE : OWNER_MAY_DO_ANYTHING;
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
    if (r == -OS_ERROR_NOENT) {
        errno = ENOENT;
    } else if (r == -OS_ERROR_FAULT) {
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

/* chmod grants the one request that is already true - the bits stat
   reports for that file - and refuses every other with EPERM, because
   nothing here would enforce a different answer (M65, M98). chmod follows a
   link, so it is the target's bits that are asked about. */
static int mode_is_what_stat_reports(mode_t mode) {
    return (mode & 07777) == OWNER_MAY_DO_ANYTHING;
}

int chmod(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (mode_is_what_stat_reports(mode)) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int fchmod(int fd, mode_t mode) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    if (mode_is_what_stat_reports(mode)) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    (void)flags;
    if (dirfd == AT_FDCWD || (path && path[0] == '/')) {
        return chmod(path, mode);
    }
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
