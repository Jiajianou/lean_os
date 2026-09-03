/* user_space/libc/src/stat.c - M77
 *
 * `struct stat` from SYS_stat's three fields, plus the constants a
 * program expects to be able to read. See <sys/stat.h> for what each
 * field is and, more importantly, which of them are zero on purpose.
 */
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

static void fill(struct stat *out, const os_stat_t *st) {
    memset(out, 0, sizeof(*out));
    /* M87: three types now, and the order matters - a link is checked
     * first because a link's own is_dir says nothing about the link. */
    out->st_mode = st->is_link ? S_IFLNK : (st->is_dir ? S_IFDIR : S_IFREG);
    out->st_size = (off_t)st->size;
    out->st_mtime = (time_t)st->mtime;
    out->st_atime = out->st_mtime; /* leanfs stores one timestamp, not three */
    out->st_ctime = out->st_mtime;
    /* M89: the nanosecond halves, zeroed explicitly. leanfs has whole
     * seconds and nothing finer, so this is the value rather than a
     * missing initialization - see <sys/stat.h>. */
    out->st_mtim.tv_nsec = 0;
    out->st_atim.tv_nsec = 0;
    out->st_ctim.tv_nsec = 0;
    out->st_rdev = 0;
    /* M89: a real inode number, which is what makes two paths
     * distinguishable as files. See <sys/stat.h> and os_stat_t for what
     * changed and for the bug that found it. */
    out->st_ino = (ino_t)st->inode;
    out->st_nlink = 1;             /* no hard links exist here, so this is a fact rather than a default */
    out->st_blksize = 4096;        /* leanfs's own block size (LEANFS_BLOCK_SIZE) - M93: was 512 */
    out->st_blocks = (blkcnt_t)((st->size + 511u) / 512u);
}

/* M98: SYS_stat is the first syscall in this ABI to carry a reason out
 * (negated, Linux-style - see OS_ERR_NOENT in system_api's syscall.h),
 * and this is where it becomes errno. Set only from what the kernel
 * actually said, which is <errno.h>'s standing rule; the program that
 * forced the question was `ar`, whose "create the archive" path begins
 * with a stat that must fail with ENOENT specifically. */
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

/* M87: a real lstat, over the syscall that does not follow a final link.
 *
 * This was an alias for `stat` for ten milestones, with an honest note
 * that "there are no symbolic links on this filesystem, so there is
 * nothing for lstat to decline to follow" - and a correct argument that
 * an alias beat a stub returning an error, because a tree walker calling
 * lstat is asking "what is this entry" and stat answered it. leanfs has
 * links now, so the two calls answer different questions and the alias
 * would give the wrong one: a walker following it would descend into
 * whatever a link pointed at, including a directory above itself. */
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
    if (!out || sys_fstat(fd, &st) != 0) {
        return -1;
    }
    fill(out, &st);
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode; /* accepted and ignored - see <sys/stat.h> */
    if (sys_mkdir(path) != 0) {
        /* M89: EEXIST when it is already there, which is what makes
         * `mkdir -p` work - see __lean_path_errno's own note for why
         * this is inferred rather than reported. */
        errno = __lean_path_errno(path, 1);
        return -1;
    }
    return 0;
}

/* ---- M89: the setters, and the one of them that is real -------------
 *
 * See <sys/stat.h> for the argument. The short version: leanfs stores no
 * mode and no owner, so a setter that returned 0 would be lying about
 * the only thing it exists to do.
 */
/* M98 amends M89's blanket refusal with the one case a modeless
 * filesystem can grant truthfully: setting the permission bits to the
 * zero this libc's own stat reports. bzip2's compress path is
 * fchmod(fd, st.st_mode) - it hands back exactly what stat gave it -
 * and ERROR_IF_NOT_ZERO around that call meant every `bzip2 -z` on
 * this machine died at the finish line... and the [m94] boot fixture
 * never noticed, because a failed -z leaves the input file untouched
 * and `cmp in keep` then passes vacuously. Asking for what is already
 * the case succeeds because it is already the case; asking for any
 * actual permission bits stays the EPERM refusal M89 argued for. */
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
    /* A named pipe is a filesystem object leanfs has no type for. M59's
     * pipes are anonymous and live in the kernel; SYS_pipe_open's named
     * pipes are a lean_os interface with its own namespace and are not
     * this. EOPNOTSUPP rather than EPERM: the operation is not permitted
     * nowhere, it is unsupported here. */
    errno = EOPNOTSUPP;
    return -1;
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    (void)path;
    (void)mode;
    (void)dev;
    /* /dev is devfs (M87) and its contents are fixed by the kernel. A
     * program cannot add a node to it, which is what this call is for. */
    errno = EOPNOTSUPP;
    return -1;
}

/* The file-creation mask: a real number this library keeps, over a
 * filesystem with no modes for it to mask. See <sys/stat.h>.
 *
 * 022 is the value every Unix starts a login shell with, and starting
 * anywhere else would make a program that reads it without setting it
 * behave differently here for no reason anybody could name. */
static mode_t umask_value = 022;

mode_t umask(mode_t mask) {
    mode_t old = umask_value;
    umask_value = mask & 07777;
    return old;
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) {
    (void)dirfd;
    return mknod(path, mode, dev); /* refused either way - see mknod */
}

int mkfifoat(int dirfd, const char *path, mode_t mode) {
    (void)dirfd;
    return mkfifo(path, mode);
}
