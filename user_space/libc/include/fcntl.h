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

#include <stddef.h>
#include <sys/types.h>
#include "syscall.h" /* system_api/include/syscall.h - the OPEN_* flags, and M84's F_*_CMD/FD_CLOEXEC_BIT */

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

#define O_RDONLY   OPEN_READ
#define O_WRONLY   OPEN_WRITE
#define O_RDWR     (OPEN_READ | OPEN_WRITE)
#define O_CREAT    OPEN_CREATE
#define O_TRUNC    OPEN_TRUNCATE
#define O_APPEND   OPEN_APPEND
/* M87: a real bit, and a real guarantee. This said "there is no
 * exclusive-create in this filesystem's open path, so a program that
 * relies on O_EXCL to avoid a race gets no protection" for twenty-eight
 * milestones. There is now: the existence check and the creation happen
 * inside one critical section of the filesystem lock, so of two
 * processes that both ask, exactly one gets the file. That is what makes
 * a lock file a lock. */
#define O_EXCL     OPEN_EXCL
/* M100: real, on sockets and pipes. It was 0 - "every descriptor here is
 * what it is" - from M88 until a TLS library's socket layer turned out
 * to be read(fd) and write(fd) on a socket, expecting both to block and
 * to say EAGAIN when told not to. The value is the kernel's
 * OS_NONBLOCK_BIT and Linux's O_NONBLOCK, so nothing translates it. A
 * file accepts the bit and never waits anyway; see SYS_fcntl. */
#define O_NONBLOCK 0x800
#define O_NDELAY   O_NONBLOCK /* the older spelling of the same flag */
/* M84: a real bit. There is an exec now, so this finally has something to
 * mean - and SYS_spawn honours it too, because a spawn is a fork and an
 * exec in one call and this flag is about the exec half.
 *
 * Note it is OPEN_CLOEXEC and not FD_CLOEXEC_BIT: the F_SETFD flag and
 * the open() flag are two different namespaces that happen to describe
 * the same property, and conflating them collides with O_RDONLY. See the
 * note next to OPEN_CLOEXEC. */
#define O_CLOEXEC  OPEN_CLOEXEC

/* M89: the access-mode mask, which a program ANDs with to recover
 * "was this opened for reading, writing or both" from a flag word.
 *
 * On Linux O_RDONLY is 0 and O_ACCMODE is 3 because the access mode is a
 * two-bit enumeration. Here it is two independent bits (OPEN_READ and
 * OPEN_WRITE), so O_RDONLY is 1 and not 0 - and the mask is still the
 * union of the two, which is what every use of it actually wants:
 * `(flags & O_ACCMODE) != O_RDONLY` is true here exactly when the
 * descriptor is writable, which is the question being asked. A program
 * that instead switches on the masked value expecting 0/1/2 gets the
 * wrong branch, and nothing can be done about that - see this header's
 * own note about hardcoded 0x241. */
#define O_ACCMODE  (OPEN_READ | OPEN_WRITE)

/* Accepted and ignored, in the sense that they describe a check this
 * open path already performs or one it cannot: O_DIRECTORY is what a
 * program passes to make sure it did not open a file by mistake, and
 * SYS_open's own type check is what actually answers that; O_NOCTTY
 * describes acquiring a controlling terminal, which nothing here does
 * on open. Defined as 0 so that a program that ORs them in compiles and
 * behaves the same way - which is true, rather than convenient. */
#define O_DIRECTORY 0
#define O_NOCTTY    0
#define O_LARGEFILE 0
#define O_NOFOLLOW  OPEN_NOFOLLOW

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
 *   F_GETFL -> the descriptor's real access mode, from the kernel, as
 *              of M98 - because in this encoding 0 is NOT O_RDONLY the
 *              way it is on Linux, it is "no access", and BFD aborts on
 *              it. O_NONBLOCK is still never set, which is still true.
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

/* ---- M89: record locks, declared and refused; M100: real ---------------
 *
 * M89 declared `struct flock` and the three lock commands so a program
 * that updates a shared file could compile, and refused all three with
 * EOPNOTSUPP - on M65's rule, because an advisory lock that always
 * succeeds is a lock that protects nothing while telling every caller
 * it did, and two processes both believing they hold one is a corrupted
 * file. That refusal held until something needed the real thing.
 *
 * sqlite did. Its unix VFS takes an F_SETLK before every transaction
 * and reports any answer but "granted" or "held by somebody else" as a
 * disk I/O error, so on this machine every INSERT failed. The locks are
 * real as of M100 (kernel/fs/flock.c): per process, per inode, byte
 * ranges with an open end, shared or exclusive, released when the
 * process closes ANY descriptor for the file or exits - POSIX's rule,
 * kept exactly because sqlite's own source depends on it. F_SETLKW
 * waits. What is NOT here is deadlock detection: nothing has asked.
 * A held lock is EAGAIN, a full table is ENOLCK, a descriptor that is
 * not a disk file is EINVAL.
 */
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7

#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2

struct flock {
    short l_type;   /* F_RDLCK / F_WRLCK / F_UNLCK */
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

/* M88: the "relative to the current directory" dirfd, defined because
 * utimensat takes one and a program has to be able to name that case.
 *
 * The rest of the *at() family - openat, fstatat, unlinkat - does not
 * exist here; it is M89's, where toybox asks for it by name. So this
 * constant is the only dirfd anything accepts, and every call that takes
 * one refuses the others rather than resolving against the wrong
 * directory, which is a silent wrong answer where a refusal is a
 * loud one. -100 is the value Linux uses and the one a ported program
 * will have baked into an object file. */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#define AT_SYMLINK_NOFOLLOW 0x100
/* M89: the rest of the flag set the *at() family takes. AT_EACCESS is
 * accepted and ignored by faccessat for the reason <unistd.h>'s access()
 * gives: this machine answers R/W/X from existence, so asking with the
 * effective ids rather than the real ones cannot produce a different
 * answer when the two are the same number. */
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EACCESS          0x200
#define AT_NO_AUTOMOUNT     0x800
#define AT_EMPTY_PATH       0x1000

int fcntl(int fd, int cmd, ...);

/* ---- M89: the *at() family ------------------------------------------
 *
 * M87 scheduled these, M93 absorbed them without landing them, and the
 * "Before M94" table handed them here because toybox asks for them by
 * name - dirtree.c walks a tree with openat and fstatat and nothing
 * else.
 *
 * **How a dirfd is resolved, which is the only interesting thing about
 * this family here.** The kernel records the absolute path a descriptor
 * was opened with (see SYS_fdpath) and these calls join it to the
 * relative name. That is a real implementation of the *ergonomics* of
 * *at() - a program can walk a tree by descriptor and every path it
 * builds is correct - and it is deliberately NOT the *guarantee* the
 * Linux versions carry, which is that the directory cannot be swapped
 * out from under the walk by a rename. On a machine with one principal
 * and no adversary between two processes, that guarantee is protecting
 * against something nobody is doing; the ergonomics are what a program
 * uses every time.
 *
 * Said out loud here rather than left implicit, because a program that
 * *is* relying on the race-freedom - a privileged file manager on a
 * multi-user system - would be silently wrong, and the day this machine
 * grows a second principal is the day this note becomes a bug report.
 */
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
