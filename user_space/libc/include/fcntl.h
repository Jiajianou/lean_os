#pragma once

#include <stddef.h>
#include <sys/types.h>
#include "syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY   OPEN_READ
#define O_WRONLY   OPEN_WRITE
#define O_RDWR     (OPEN_READ | OPEN_WRITE)
#define O_CREAT    OPEN_CREATE
#define O_TRUNC    OPEN_TRUNCATE
#define O_APPEND   OPEN_APPEND
#define O_EXCL     OPEN_EXCL
#define O_NONBLOCK 0x800
#define O_NDELAY   O_NONBLOCK
#define O_CLOEXEC  OPEN_CLOEXEC

#define O_ACCMODE  (OPEN_READ | OPEN_WRITE)

#define O_DIRECTORY 0
#define O_NOCTTY    0
#define O_LARGEFILE 0
#define O_NOFOLLOW  OPEN_NOFOLLOW

int open(const char *path, int flags, ...);

int creat(const char *path, mode_t mode);

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define FD_CLOEXEC 1

#define F_DUPFD_CLOEXEC 8

/* M120 built sealing as memfd_add_seals and memfd_seals. These are the same
   two operations under the names portable code reaches for; fcntl routes
   them to that one syscall rather than inventing a second path. */
/* posix_fadvise is advisory BY DEFINITION - POSIX says an implementation may
   ignore the hint - so returning success without acting on it is conformant
   rather than the kind of no-op M65 refuses. What would make it worth acting
   on is a block cache that took a hint; kernel/drivers/blk.c's does not have
   one yet. */
#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_RANDOM     1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5

int posix_fadvise(int fd, off_t offset, off_t length, int advice);

/* fallocate's modes. Only mode zero - make sure the space is there - is
   something this filesystem can carry out; punching a hole and the rest are
   refused rather than accepted, because a caller that asked for a hole and
   got success would believe the file had one. */
#define FALLOC_FL_KEEP_SIZE  0x01
#define FALLOC_FL_PUNCH_HOLE 0x02

int fallocate(int fd, int mode, off_t offset, off_t length);
int posix_fallocate(int fd, off_t offset, off_t length);

#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034

#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7

#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EACCESS          0x200
#define AT_NO_AUTOMOUNT     0x800
#define AT_EMPTY_PATH       0x1000

int fcntl(int fd, int cmd, ...);

int openat(int dirfd, const char *path, int flags, ...);
int mkdirat(int dirfd, const char *path, mode_t mode);
int unlinkat(int dirfd, const char *path, int flags);
int renameat(int oldfd, const char *oldpath, int newfd, const char *newpath);
int symlinkat(const char *target, int dirfd, const char *path);
int linkat(int oldfd, const char *oldpath, int newfd, const char *newpath, int flags);
long readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
int faccessat(int dirfd, const char *path, int mode, int flags);

#ifdef __cplusplus
}
#endif
