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

#include "syscall.h" /* system_api/include/syscall.h - the OPEN_* flags, and M84's F_*_CMD/FD_CLOEXEC_BIT */

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
/* M84: a real bit. There is an exec now, so this finally has something to
 * mean - and SYS_spawn honours it too, because a spawn is a fork and an
 * exec in one call and this flag is about the exec half.
 *
 * Note it is OPEN_CLOEXEC and not FD_CLOEXEC_BIT: the F_SETFD flag and
 * the open() flag are two different namespaces that happen to describe
 * the same property, and conflating them collides with O_RDONLY. See the
 * note next to OPEN_CLOEXEC. */
#define O_CLOEXEC  OPEN_CLOEXEC

int open(const char *path, int flags, ...);

/* ---- fcntl, and exactly how little it can honestly do ------------------
 *
 * fcntl carries commands about two per-descriptor flags. As of M84 this
 * machine has one of them for real and still does not have the other,
 * and the difference is worth stating command by command rather than in
 * a summary that would be half wrong:
 *
 *   F_GETFD / F_SETFD -> real, as of M84. There IS an exec on this
 *              machine now (SYS_execve), so FD_CLOEXEC has something to
 *              mean: a descriptor marked with it does not survive into
 *              the next program, and SYS_spawn honours it too because a
 *              spawn is a fork and an exec in one call. This paragraph
 *              said the opposite for seven milestones and the note above
 *              said the header exists to avoid exactly that.
 *   F_GETFL -> 0. O_NONBLOCK is 0 here - see the flags above - so no
 *              status flag is set either.
 *   F_SETFL -> 0 if the caller is setting nothing, -1 otherwise.
 *              Accepting a flag that will not be honoured is the failure
 *              mode this whole file is written to avoid, and O_NONBLOCK
 *              is still a flag this system cannot honour. M88.
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
