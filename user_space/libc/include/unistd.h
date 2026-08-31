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
#include <sys/types.h>

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
/* M87: it arrived. The paragraph here used to say "no ftruncate... a
 * declaration with no implementation would be worse than its absence: a
 * program that probes for it at configure time would find it and then
 * fail to link. It arrives the day something asks." There is a syscall
 * behind it now (SYS_ftruncate), and it does more than the function that
 * note described: growing a file only changes its size, because an
 * unallocated block already reads as zeros. */
int ftruncate(int fd, off_t length);

int unlink(const char *path);
int pipe(int fds[2]);

/* M83: the real thing. Returns the child's pid in the parent, 0 in the
 * child, and -1 if the fork failed.
 *
 * The paragraph that used to be below this one said "there is no fork on
 * this machine", and it was true for eighty-two milestones. The child is
 * a copy-on-write clone: it shares every page with its parent until one
 * of them writes, which is what makes this affordable and what M82's
 * page-fault handler had to exist first for. */
pid_t fork(void);

/* M84: replaces this program with another. Does not return on success,
 * which is why every caller in the world writes the error path with no
 * `if` around it. `execv` supplies the current environment; `execvp`
 * searches PATH when `file` contains no '/'.
 *
 * A `#!` script is refused: the kernel does not resolve shebangs (see
 * SYS_execve), and a program that wants to run a script can run its
 * interpreter. The shell resolves them, which is where M72 put that job
 * and where it belongs. */
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);

/* Runs `path` with `argv` and, for the `e` form, `envp`; returns the new
 * process's pid rather than replacing this one.
 *
 * Still not execve, and still useful: SYS_spawn is a combined fork+exec
 * (see system_api/include/syscall.h) and remains the cheap path for the
 * overwhelmingly common case of "start this program", which is what every
 * launcher on this desktop actually wants. `fork` above is for the cases
 * that need the two halves apart. M84 is where `execve` lands and where
 * this stops being the only way to start a program with arguments. */
long spawnv(const char *path, char *const argv[]);
long spawnve(const char *path, char *const argv[], char *const envp[]);
