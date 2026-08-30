/* user_space/libc/include/fcntl.h - M80 groundwork
 *
 * `open` and its flags, in the spelling a program written elsewhere
 * uses. The values are this project's own OPEN_* (system_api/include/
 * syscall.h) under POSIX names rather than a second set: one bit, one
 * meaning, one place it is defined.
 *
 * That means the numbers are NOT Linux's, and a program that hardcodes
 * 0x241 instead of O_WRONLY|O_CREAT|O_TRUNC will be wrong here. Nothing
 * can be done about that - the kernel's flags predate this header by
 * twenty milestones - and it is better said than discovered.
 */
#pragma once

#include "syscall.h" /* system_api/include/syscall.h - the OPEN_* flags themselves */

#define O_RDONLY   OPEN_READ
#define O_WRONLY   OPEN_WRITE
#define O_RDWR     (OPEN_READ | OPEN_WRITE)
#define O_CREAT    OPEN_CREATE
#define O_TRUNC    OPEN_TRUNCATE
#define O_APPEND   OPEN_APPEND
/* Accepted and ignored: there is no exclusive-create in this
 * filesystem's open path, so a program that relies on O_EXCL to avoid a
 * race gets no protection. Zero rather than a bit nothing checks, so
 * that at least the flag word is honest. */
#define O_EXCL     0
#define O_NONBLOCK 0 /* every descriptor here is what it is - see SYS_read/SYS_recv */
#define O_CLOEXEC  0 /* there is no exec, only SYS_spawn, which copies the table */

int open(const char *path, int flags, ...);

/* ---- fcntl, and exactly how little it can honestly do ------------------
 *
 * Every command fcntl carries is about a per-descriptor flag this kernel
 * does not have. That does not make the function meaningless, because
 * "this flag is not set" is a true answer for two of them and the only
 * ones a portable program actually reaches for:
 *
 *   F_GETFD -> 0. There is no exec on this machine (SYS_spawn loads a
 *              fresh image and copies the fd table), so FD_CLOEXEC has
 *              nothing to mean and is genuinely not set.
 *   F_GETFL -> 0. O_NONBLOCK is 0 here - see the flags above - so no
 *              status flag is set either.
 *   F_SETFD / F_SETFL -> 0 if the caller is setting nothing, -1
 *              otherwise. Accepting a flag that will not be honoured is
 *              the failure mode this whole file is written to avoid.
 *
 * Everything else is -1. A program that needs F_DUPFD has dup2().
 */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define FD_CLOEXEC 1

int fcntl(int fd, int cmd, ...);
