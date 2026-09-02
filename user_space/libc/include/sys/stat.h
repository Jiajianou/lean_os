/* user_space/libc/include/sys/stat.h - M77
 *
 * A real `struct stat` over SYS_stat, which since M59 has reported the
 * three fields the file manager's columns are made of and nothing a
 * general-purpose program goes looking for.
 *
 * What `st_mode` does and does not say, because this is the field a
 * reader will assume is lying:
 *
 *   - the FILE TYPE bits are real. S_ISDIR and S_ISREG answer correctly,
 *     because leanfs genuinely distinguishes the two and SYS_stat
 *     genuinely reports which.
 *   - the PERMISSION bits are 0, deliberately, and a program that tests
 *     them will find nothing set rather than a plausible 0644. There are
 *     no users on this machine (M65 argued that at length and refused to
 *     invent one) and nothing here enforces a mode. Filling in bits
 *     nobody checks would be exactly the invented fiction M65 declined
 *     to write for uids; a zero says "this system has no answer", which
 *     is true.
 *
 * `st_uid`, `st_gid` and `st_nlink` are here for the same reason - a
 * program that reads them compiles - and hold 0, 0 and 1. There are no
 * hard links in leanfs, so st_nlink is 1 by construction rather than by
 * convention.
 */
#pragma once

#include <sys/types.h>
#include <time.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000 /* M87 - see S_ISLNK */

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
/* M87: a real bit. This said "no symbolic links here, and saying so beats
 * a bit that is never set" for ten milestones; leanfs has them now.
 *
 * Only `lstat` can ever set it: `stat` follows a link, so by the time it
 * answers there is nothing left to report - which is exactly why the two
 * calls exist and why `ls -l` uses the one it does. */
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
/* M89: the four types leanfs cannot hold, given numbers so that a
 * program which switches on S_IFMT compiles and takes its "none of the
 * above" branch. Their test macros compare against the real bits rather
 * than expanding to 0 - which is the same answer today and stops being a
 * lie the day a device node has a mode. */
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000

#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* The permission bits, defined so a program that ORs them together
 * compiles. Nothing on this machine reads them - see the header note. */
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

/* ---- M89: the timestamps are timespecs, and st_atime is a macro ------
 *
 * POSIX.1-2008 made the three time fields `struct timespec` and defined
 * `st_atime` as a macro for `st_atim.tv_sec`, which is why code written
 * this century uses both spellings interchangeably and why toybox's
 * `cp -p` reads `st_mtim` directly.
 *
 * The nanoseconds are always 0 and that is a statement about leanfs
 * rather than about this header: an inode stores one 32-bit second
 * count, so a nanosecond field with anything in it would be invented.
 * See <time.h>'s note on the same question for the clock.
 */
struct stat {
    mode_t   st_mode;   /* file type bits only - see the header comment */
    off_t    st_size;
    struct timespec st_mtim;
    struct timespec st_atim;  /* == st_mtim: leanfs stores one timestamp, not three */
    struct timespec st_ctim;  /* == st_mtim, same reason */
    nlink_t  st_nlink;  /* always 1 - leanfs has no hard links */
    /* M89: a real inode number.
     *
     * This said "0 - see <dirent.h>'s note on d_ino" for twelve
     * milestones, which meant every file on this machine had the same
     * identity - so every program that compares (st_dev, st_ino) to ask
     * "are these the same file" got yes for every pair. toybox's `cp`
     * asks that before copying and refused to copy anything at all,
     * which is how it was found: by a program nobody here wrote, on the
     * first day one ran.
     *
     * st_dev below is still 0 and still cannot be otherwise; the
     * synthetic mounts keep out of leanfs's number range instead. See
     * kernel/fs/devfs.c's DEVFS_INO_BASE. */
    ino_t    st_ino;
    dev_t    st_dev;    /* 0 - one filesystem, no device numbers to distinguish it from */
    /* The device a special file names. There are no special files on
     * leanfs, so this is 0 for everything - and it is a separate field
     * from st_dev rather than an alias, because a program that prints
     * both would otherwise print the same number twice and look right. */
    dev_t    st_rdev;
    uid_t    st_uid;    /* 0 - there are no users here (M65) */
    gid_t    st_gid;    /* 0 */
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
};

/* The pre-2008 spelling, as POSIX itself defines it: a macro onto the
 * seconds field. Every existing caller in this tree reads `.st_mtime`
 * and keeps working unchanged, which is the property that made this a
 * safe change rather than a sweep. */
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

int stat(const char *path, struct stat *out);
int fstat(int fd, struct stat *out);
/* M87: a real lstat over SYS_lstat - it does not follow a final symbolic
 * link, and reports S_IFLNK for one. The comment that stood here until
 * M88 said no symbolic links existed on this filesystem and that the two
 * calls therefore could not differ. That was true when it was written
 * and stopped being true one milestone later; see stat.c. */
int lstat(const char *path, struct stat *out);

/* `mode` is accepted and ignored, for the reason the header note gives.
 * Taking it and doing nothing is better than not taking it: a program
 * that passes 0755 compiles and behaves identically, and one that reads
 * the mode back gets the honest 0. */
int mkdir(const char *path, mode_t mode);

/* M88: the two nanosecond sentinels utimensat takes, and the call
 * itself. leanfs stores whole seconds, so a real tv_nsec is truncated -
 * see utime.c. UTIME_OMIT asks this call to leave the one field it can
 * set alone, which is a success that does nothing. */
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

int utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags);

/* M89: fstatat, whose implementation is in fcntl.c with the rest of the
 * *at() family because it is the same path join. AT_SYMLINK_NOFOLLOW
 * selects lstat over stat, which is the only flag it has. */
int fstatat(int dirfd, const char *path, struct stat *out, int flags);

/* ---- M89: the mode and ownership setters, which all refuse ------------
 *
 * `chmod` has been described as "a truthful failure" in three headers
 * since M65 without ever existing as a symbol. Toybox calls fchmod,
 * fchmodat and fchown by name, so they arrive - and they arrive as the
 * refusal those notes describe rather than as a no-op returning 0.
 *
 * Why a refusal and not a success: leanfs stores no mode and no owner,
 * so a call that returned 0 would be telling a program that a file is
 * now 0600 when reading it back reports 0. That is the fiction M65
 * declined to write, and it is worse in the setter direction than in the
 * getter one - a backup tool that "restored" permissions and a `cp -p`
 * that reported success would both be lying about the thing they exist
 * to preserve.
 *
 * EPERM rather than ENOSYS, because the call exists and the operation is
 * refused; ENOSYS would tell a configure script to stop probing for a
 * function that is genuinely here.
 */
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
int fchmodat(int dirfd, const char *path, mode_t mode, int flags);
int mkfifo(const char *path, mode_t mode);
int mknod(const char *path, mode_t mode, dev_t dev);
int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev);
int mkfifoat(int dirfd, const char *path, mode_t mode);

/* M89: utimensat's descriptor form. leanfs stores whole seconds, so a
 * real tv_nsec is truncated - see utime.c, which says the same thing
 * about the path form. */
int futimens(int fd, const struct timespec ts[2]);

/* M89: the process file-creation mask.
 *
 * This one is NOT a refusal, and the difference from chmod above is
 * worth the paragraph. umask does not claim anything about a file: it is
 * a number a process keeps and subtracts from the modes it asks for, and
 * every program that reads it back gets exactly what it set. So this is
 * a real implementation of a real POSIX object - it lives in libc,
 * because the kernel has nothing to do with it here, and it is inherited
 * across fork the way any other libc variable is and reset by exec the
 * way any other program's data is.
 *
 * What it does not do is affect the modes of created files, because
 * there are none. A program that sets 022 and creates a file finds mode
 * 0, same as before. That is the honest half-answer: the mask is real
 * and the thing it would mask is absent.
 */
mode_t umask(mode_t mask);

#ifdef __cplusplus
}
#endif
