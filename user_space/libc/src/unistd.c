/* user_space/libc/src/unistd.c - M75
 *
 * <unistd.h>'s implementations. Every one is a thin renaming of a
 * syscall wrapper that already existed, which is the whole point: a
 * program somebody else wrote calls `chdir`, `write` and `getcwd`, not
 * `sys_chdir`, `sys_write` and `sys_getcwd`, and the gap between those
 * two spellings is the only thing that was stopping it.
 */
#include <fcntl.h>
#include <unistd.h>

#include "syscall_wrappers.h"

int chdir(const char *path) {
    return (int)sys_chdir(path);
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) {
        return 0;
    }
    /* Refused rather than truncated by the kernel, so a NULL here means
     * "your buffer is too small", which is exactly what a caller has to
     * know in order to try a bigger one. */
    if (sys_getcwd(buf, size) < 0) {
        return 0;
    }
    return buf;
}

int getpid(void) {
    return (int)sys_getpid();
}

long read(int fd, void *buf, size_t count) {
    return sys_read(fd, buf, count);
}

long write(int fd, const void *buf, size_t count) {
    return sys_write(fd, buf, count);
}

int close(int fd) {
    return (int)sys_close(fd);
}

int access(const char *path, int mode) {
    os_stat_t st;
    if (sys_stat(path, &st) != 0) {
        return -1;
    }
    /* Existence is the only question this machine can answer honestly -
     * see the note in <unistd.h>. Anything that exists is reachable, and
     * whether an operation is permitted is decided by the caller's
     * capability set at the moment it tries. */
    (void)mode;
    return 0;
}

int rmdir(const char *path) {
    return (int)sys_rmdir(path);
}

int unlink(const char *path) {
    return (int)sys_unlink(path);
}

long spawnv(const char *path, char *const argv[]) {
    return sys_spawnv(path, (const char *const *)argv);
}

long spawnve(const char *path, char *const argv[], char *const envp[]) {
    return sys_spawnve(path, (const char *const *)argv, (const char *const *)envp);
}

/* ---- M80 groundwork ---------------------------------------------------- */

int isatty(int fd) {
    /* fd 0 and 1 are the implicit stdin/stdout every task starts with
     * (kernel/sched/sched.h's fd table); everything else is a pipe, a
     * socket or a file. There is no terminal device to ask, so this is
     * the honest approximation - and it is right for the one thing a
     * ported program uses it for, which is deciding whether to prompt.
     *
     * A GUI terminal's child has had fd 0 closed deliberately
     * (gui_terminal.c), and a read on it fails - so a program that asks
     * and then reads gets a consistent answer. */
    return (fd == 0 || fd == 1) ? 1 : 0;
}

long lseek(int fd, long offset, int whence) {
    return sys_lseek(fd, offset, whence);
}

int dup2(int oldfd, int newfd) {
    return (int)sys_dup2(oldfd, newfd);
}

int pipe(int fds[2]) {
    return (int)sys_pipe(fds);
}

/* <fcntl.h>'s open. Variadic to match POSIX's `mode` argument, which is
 * accepted and ignored for the reason <sys/stat.h> gives about
 * permission bits. */
int open(const char *path, int flags, ...) {
    return (int)sys_open(path, (uint32_t)flags);
}

/* See <fcntl.h> for why this answers what it answers. */
int fcntl(int fd, int cmd, ...) {
    if (fd < 0) {
        return -1;
    }
    switch (cmd) {
    case F_GETFD:
    case F_GETFL:
        return 0; /* genuinely no flag set - see the header */
    case F_SETFD:
    case F_SETFL: {
        /* Setting nothing succeeds; setting anything is refused, because
         * this system cannot honour it and a silent success would be a
         * program believing its descriptor is non-blocking. */
        __builtin_va_list ap;
        __builtin_va_start(ap, cmd);
        int arg = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        return arg == 0 ? 0 : -1;
    }
    default:
        return -1;
    }
}

pid_t fork(void) {
    return (pid_t)sys_fork();
}
