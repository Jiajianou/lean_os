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
#define S_ISCHR(m) (((void)(m), 0))
#define S_ISBLK(m) (((void)(m), 0))
#define S_ISFIFO(m) (((void)(m), 0))

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

struct stat {
    mode_t   st_mode;   /* file type bits only - see the header comment */
    off_t    st_size;
    time_t   st_mtime;
    time_t   st_atime;  /* == st_mtime: leanfs stores one timestamp, not three */
    time_t   st_ctime;  /* == st_mtime, same reason */
    nlink_t  st_nlink;  /* always 1 - leanfs has no hard links */
    ino_t    st_ino;    /* 0 - see <dirent.h>'s note on d_ino */
    dev_t    st_dev;    /* 0 - one filesystem, no device numbers to distinguish it from */
    uid_t    st_uid;    /* 0 - there are no users here (M65) */
    gid_t    st_gid;    /* 0 */
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
};

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
