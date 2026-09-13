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
