/* user_space/libc/include/unistd.h - M75
 *
 * The POSIX names for things this kernel already does, plus the two
 * M75 adds. See string.h's header comment for why this project ships its
 * own headers rather than borrowing a libc's.
 *
 * Deliberately small, and it grows the way M63's rule says: when a
 * program somebody else wrote fails to link without a name, not when a
 * standard lists one. What is here is what "a place to stand" needs -
 * `chdir`/`getcwd`, `environ` - alongside the read/write/close that were
 * already syscalls under a different spelling.
 */
#pragma once

#include <stddef.h>

/* The environment, as every program that has ever walked one expects to
 * find it: a NULL-terminated array of "NAME=value". Points into the
 * kernel's argument region until something calls setenv/putenv, at which
 * point libc/src/env.c moves it onto the heap - see that file. */
extern char **environ;

/* M75. `chdir` refuses anything that is not an existing directory, so a
 * cd that succeeded and one that quietly did nothing cannot be confused.
 * `getcwd` returns `buf` on success and NULL if the directory does not
 * fit - refusing rather than truncating, because a truncated path names
 * a different directory. */
int chdir(const char *path);
char *getcwd(char *buf, size_t size);

int getpid(void);

/* M77. `mode` is F_OK / R_OK / W_OK / X_OK, and only F_OK is answered
 * from anything real: this machine has no permission bits (see
 * <sys/stat.h>), so R/W/X are reported as granted for anything that
 * exists. That is not a stub - it is the true answer on a system where
 * what a process may do is decided by its capability set (M65) and never
 * by a file mode. A program that wants to know whether it may write
 * should try, and read the error. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char *path, int mode);

int rmdir(const char *path);
int unlink(const char *path);

long read(int fd, void *buf, size_t count);
long write(int fd, const void *buf, size_t count);
int close(int fd);
/* M80 groundwork. `isatty` answers from what this system actually knows:
 * fd 0 and 1 are the implicit stdin/stdout every task starts with
 * (kernel/sched/sched.h's fd table), and everything else is a pipe, a
 * socket or a file. There is no terminal device here to ask, so this is
 * the honest approximation and not a stub - it is right for every use a
 * ported program puts it to (deciding whether to prompt). */
int isatty(int fd);
long lseek(int fd, long offset, int whence);
int dup2(int oldfd, int newfd);
/* No ftruncate. leanfs can truncate an open handle to zero
 * (leanfs_handle_truncate) and there is no syscall that exposes it, and
 * a declaration with no implementation would be worse than its absence:
 * a program that probes for it at configure time would find it and then
 * fail to link. It arrives the day something asks. */
int unlink(const char *path);
int pipe(int fds[2]);

/* Runs `path` with `argv` and, for the `e` form, `envp`; returns the new
 * process's pid rather than replacing this one. NOT execve: there is no
 * fork on this machine and SYS_spawn is a combined fork+exec (see
 * system_api/include/syscall.h), so a call that never returned would be
 * a call nothing could use. Named for what it does. */
long spawnv(const char *path, char *const argv[]);
long spawnve(const char *path, char *const argv[], char *const envp[]);
