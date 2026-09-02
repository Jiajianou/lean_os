/* user_space/libc/include/sys/statvfs.h - M88
 *
 * How big the filesystem behind a path is, and how much of it is left.
 *
 * Both counts are real here, which is worth saying because f_files and
 * f_ffree are the fields most often filled with zeros: leanfs has a
 * fixed inode table sized at format time, so a tree of small files runs
 * out of inodes with most of the disk still free. A caller watching only
 * f_bavail would watch the ceiling this machine does not hit first.
 *
 * f_bfree and f_bavail are the same number, and that is a fact rather
 * than a shortcut - there is no reserved-for-root pool here, because
 * there is no root to reserve it for (see M65 on why this machine has
 * one principal and says so).
 */
#pragma once

#include <stdint.h>

typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;

struct statvfs {
    unsigned long f_bsize;   /* the filesystem's block size */
    unsigned long f_frsize;  /* fragment size; the same, since leanfs has no fragments */
    fsblkcnt_t f_blocks;     /* total data blocks */
    fsblkcnt_t f_bfree;      /* free blocks */
    fsblkcnt_t f_bavail;     /* free blocks available to a caller - see the header note */
    fsfilcnt_t f_files;      /* total inodes */
    fsfilcnt_t f_ffree;      /* free inodes */
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

int statvfs(const char *path, struct statvfs *buf);

/* Answered by asking about the path the descriptor was opened with,
 * which this libc does not remember - so this is refused rather than
 * guessed. See the note in statvfs.c. */
int fstatvfs(int fd, struct statvfs *buf);
