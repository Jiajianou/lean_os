#include <fcntl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>

#include "syscall_wrappers.h"
#include <stdlib.h>
#include <sys/wait.h>
#include "paths.h"
#include <errno.h>
#include <stdint.h>
#include "os_file_system.h"
#include <sys/random.h>
#include <limits.h>
#include "process.h"
#include <dirent.h>
#include <string.h>
#include <sys/resource.h>

int __lean_fd_errno(int fd) {
    os_stat_t st;
    return sys_fstat(fd, &st) == 0 ? EIO : EBADF;
}

int __lean_path_errno(const char *path, int creating) {
    if (!path || !path[0]) {
        return EFAULT;
    }
    os_stat_t st;
    if (sys_stat(path, &st) == 0) {
        return creating ? EEXIST : EACCES;
    }
    char parent[PATH_MAX];
    size_t n = 0;
    while (path[n] && n < sizeof(parent) - 1) {
        parent[n] = path[n];
        n++;
    }
    parent[n] = '\0';
    while (n > 1 && parent[n - 1] == '/') {
        n--;
    }
    while (n > 0 && parent[n - 1] != '/') {
        n--;
    }
    while (n > 1 && parent[n - 1] == '/') {
        n--;
    }
    parent[n ? n : 1] = '\0';
    if (n == 0) {
        parent[0] = '.';
        parent[1] = '\0';
    }
    if (sys_stat(parent, &st) != 0) {
        return ENOENT;
    }
    return st.is_directory ? ENOENT : ENOTDIR;
}

int chdir(const char *path) {
    if (sys_chdir(path) != 0) {
        os_stat_t st;
        if (sys_stat(path, &st) == 0 && !st.is_directory) {
            errno = ENOTDIR;
        } else {
            errno = __lean_path_errno(path, 0);
        }
        return -1;
    }
    return 0;
}

char *getcwd(char *buffer, size_t size) {
    if (!buffer || size == 0) {
        return 0;
    }
    if (sys_getcwd(buffer, size) < 0) {
        return 0;
    }
    return buffer;
}

int getpid(void) {
    return (int)sys_getpid();
}

long read(int fd, void *buffer, size_t count) {
    long r = sys_read(fd, buffer, count);
    if (r == -OS_ERROR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (r == -OS_ERROR_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (r < 0) {
        errno = __lean_fd_errno(fd);
    }
    return r;
}

ssize_t pread(int fd, void *buffer, size_t count, off_t offset) {
    long r = sys_pread(fd, buffer, count, (long)offset);
    if (r == -OS_ERROR_SPIPE) {
        errno = ESPIPE;
        return -1;
    }
    if (r < 0) {
        errno = __lean_fd_errno(fd);
        return -1;
    }
    return r;
}

ssize_t pwrite(int fd, const void *buffer, size_t count, off_t offset) {
    long r = sys_pwrite(fd, buffer, count, (long)offset);
    if (r == -OS_ERROR_SPIPE) {
        errno = ESPIPE;
        return -1;
    }
    if (r < 0) {
        errno = __lean_fd_errno(fd);
        return -1;
    }
    return r;
}

long write(int fd, const void *buffer, size_t count) {
    long r = sys_write(fd, buffer, count);
    if (r == -OS_ERROR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (r == -OS_ERROR_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (r < 0) {
        errno = __lean_fd_errno(fd);
    }
    return r;
}

void _exit(int status) {
    sys_exit(status);
    for (;;) {
    }
}

int close(int fd) {
    long r = sys_close(fd);
    if (r < 0) {
        errno = EBADF;
        return -1;
    }
    return (int)r;
}

int access(const char *path, int mode) {
    os_stat_t st;
    if (sys_stat(path, &st) != 0) {
        return -1;
    }
    (void)mode;
    return 0;
}

int rmdir(const char *path) {
    if (sys_rmdir(path) != 0) {
        os_stat_t st;
        if (sys_stat(path, &st) == 0) {
            errno = st.is_directory ? ENOTEMPTY : ENOTDIR;
        } else {
            errno = __lean_path_errno(path, 0);
        }
        return -1;
    }
    return 0;
}

int unlink(const char *path) {
    if (sys_unlink(path) != 0) {
        os_stat_t st;
        if (sys_stat(path, &st) == 0 && st.is_directory) {
            errno = EISDIR;
        } else {
            errno = __lean_path_errno(path, 0);
        }
        return -1;
    }
    return 0;
}

long spawnv(const char *path, char *const argv[]) {
    return sys_spawnv(path, (const char *const *)argv);
}

long spawnve(const char *path, char *const argv[], char *const envp[]) {
    return sys_spawnve(path, (const char *const *)argv, (const char *const *)envp);
}

int isatty(int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

long lseek(int fd, long offset, int whence) {
    long r = sys_lseek(fd, offset, whence);
    if (r < 0) {
        os_stat_t st;
        errno = sys_fstat(fd, &st) == 0 ? ESPIPE : EBADF;
    }
    return r;
}

int dup2(int oldfd, int newfd) {
    return (int)sys_dup2(oldfd, newfd);
}

pid_t gettid(void) {
    return (pid_t)sys_gettid();
}

int pipe(int file_descriptors[2]) {
    return (int)sys_pipe(file_descriptors);
}

/* The flags are applied after the pipe exists rather than atomically with
   it. That difference matters on a machine where another thread can fork
   between the two - and this one can - so the ends are set close-on-exec
   before anything else is allowed to run only in the sense that no syscall
   here yields. The condition for making it one operation is a fork that can
   interleave with this, which is a kernel change rather than a libc one. */
int pipe2(int file_descriptors[2], int flags) {
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(file_descriptors) != 0) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (flags & O_CLOEXEC) {
            if (fcntl(file_descriptors[i], F_SETFD, FD_CLOEXEC) != 0) {
                close(file_descriptors[0]);
                close(file_descriptors[1]);
                return -1;
            }
        }
        if (flags & O_NONBLOCK) {
            int current = fcntl(file_descriptors[i], F_GETFL, 0);
            if (current < 0 ||
                fcntl(file_descriptors[i], F_SETFL, current | O_NONBLOCK) != 0) {
                close(file_descriptors[0]);
                close(file_descriptors[1]);
                return -1;
            }
        }
    }
    return 0;
}

int open(const char *path, int flags, ...) {
    int fd = (int)sys_open(path, (uint32_t)flags);
    if (fd < 0) {
        errno = __lean_path_errno(path, (flags & O_CREAT) != 0);
    }
    return fd;
}

int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int fcntl(int fd, int command, ...) {
    if (fd < 0) {
        return -1;
    }
    switch (command) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        __builtin_va_list ap;
        __builtin_va_start(ap, command);
        int lowest = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        if (lowest < 0) {
            errno = EINVAL;
            return -1;
        }
        for (int i = lowest; i < OPEN_MAX; i++) {
            if (i == fd) {
                continue;
            }
            if (sys_fcntl(i, F_GETFD_COMMAND, 0) >= 0) {
                continue;
            }
            int copy = dup2(fd, i);
            if (copy < 0) {
                return -1;
            }
            if (command == F_DUPFD_CLOEXEC) {
                sys_fcntl(copy, F_SETFD_COMMAND, FD_CLOEXEC);
            }
            return copy;
        }
        errno = EMFILE;
        return -1;
    }
    case F_GETFD:
        return (int)sys_fcntl(fd, F_GETFD_COMMAND, 0);
    case F_SETFD: {
        __builtin_va_list ap;
        __builtin_va_start(ap, command);
        int arg = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        return (int)sys_fcntl(fd, F_SETFD_COMMAND, arg & FD_CLOEXEC);
    }
    case F_GETFL:
        return (int)sys_fcntl(fd, F_GETFL_COMMAND, 0);
    case F_SETFL: {
        __builtin_va_list ap;
        __builtin_va_start(ap, command);
        int arg = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        if (sys_fcntl(fd, F_SETFL_COMMAND, arg & O_NONBLOCK) < 0) {
            errno = EBADF;
            return -1;
        }
        return 0;
    }
    /* M120 built these as memfd_add_seals and memfd_seals. fcntl is the
       spelling portable code uses; it reaches the same syscall rather than a
       second implementation of sealing. */
    case F_ADD_SEALS: {
        __builtin_va_list ap;
        __builtin_va_start(ap, command);
        unsigned int seals = __builtin_va_arg(ap, unsigned int);
        __builtin_va_end(ap);
        return memfd_add_seals(fd, seals);
    }
    case F_GET_SEALS:
        return memfd_seals(fd);
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW: {
        __builtin_va_list ap;
        __builtin_va_start(ap, command);
        struct flock *fl = __builtin_va_arg(ap, struct flock *);
        __builtin_va_end(ap);
        if (!fl) {
            errno = EINVAL;
            return -1;
        }
        os_flock_t k;
        k.type = fl->l_type;
        k.whence = fl->l_whence;
        k.pid = 0;
        k.start = (int64_t)fl->l_start;
        k.length = (int64_t)fl->l_len;
        int kcmd = command == F_GETLK ? F_GETLK_COMMAND : command == F_SETLK ? F_SETLK_COMMAND : F_SETLKW_COMMAND;
        long r = sys_fcntl(fd, kcmd, (long)(uintptr_t)&k);
        if (r == 0) {
            if (command == F_GETLK) {
                fl->l_type = k.type;
                fl->l_whence = k.whence;
                fl->l_start = (off_t)k.start;
                fl->l_len = (off_t)k.length;
                fl->l_pid = (pid_t)k.pid;
            }
            return 0;
        }
        errno = r == -2 ? EAGAIN : r == -3 ? ENOLCK : EINVAL;
        return -1;
    }
    default:
        return -1;
    }
}

pid_t fork(void) {
    __lean_pthread_atfork_prepare();
    long r = sys_fork();
    if (r < 0) {
        __lean_pthread_atfork_parent();
        errno = EAGAIN;
        return -1;
    }
    if (r == 0) {
        __lean_pthread_atfork_child();
    } else {
        __lean_pthread_atfork_parent();
    }
    return (pid_t)r;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    long r = sys_execve(path, argv, envp);
    if (r < 0) {
        errno = __lean_path_errno(path, 0);
        return -1;
    }
    return (int)r;
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    if (!file || !*file) {
        return -1;
    }
    for (const char *c = file; *c; c++) {
        if (*c == '/') {
            return execve(file, argv, environ);
        }
    }

    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = PATH_DEFAULT;
    }
    char attempt[PATH_MAX_LENGTH];
    const char *p = path;
    while (*p) {
        size_t n = 0;
        while (*p && *p != ':' && n < sizeof(attempt) - 2) {
            attempt[n++] = *p++;
        }
        while (*p && *p != ':') {
            p++;
        }
        if (n == 0) {
            attempt[n++] = '.';
        }
        if (attempt[n - 1] != '/') {
            attempt[n++] = '/';
        }
        size_t f = 0;
        while (file[f] && n < sizeof(attempt) - 1) {
            attempt[n++] = file[f++];
        }
        attempt[n] = '\0';
        if (!file[f]) {
            execve(attempt, argv, environ);
        }
        if (*p == ':') {
            p++;
        }
    }
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)sys_waitpid(pid, status, options);
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage) {
    pid_t r = waitpid(pid, status, options);
    if (usage) {
        if (getrusage(RUSAGE_CHILDREN, usage) != 0) {
            memset(usage, 0, sizeof(*usage));
        }
    }
    return r;
}

pid_t wait3(int *status, int options, struct rusage *usage) {
    return wait4(-1, status, options, usage);
}

int ftruncate(int fd, off_t length) {
    return (int)sys_ftruncate(fd, (long)length);
}

/* off_t is already 64 bits here, so this IS ftruncate under the large-file
   name - the same function, not a wider one. */
int ftruncate64(int fd, off_t length) {
    return ftruncate(fd, length);
}

/* There is no path-based truncate syscall behind this: the kernel resizes a
   file through a descriptor, so this opens one. The difference that buys is
   that it is two operations rather than one - a path replaced between the
   open and the resize would be truncated by its old identity. Nothing here
   needs that to be atomic, and inventing a syscall for it would be building
   a second way to do the same thing. */
int truncate(const char *path, off_t length) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    int r = ftruncate(fd, length);
    int saved = errno;
    close(fd);
    if (r != 0) {
        errno = saved;
    }
    return r;
}

int symlink(const char *target, const char *path) {
    if (sys_symlink(target, path) != 0) {
        errno = __lean_path_errno(path, 1);
        return -1;
    }
    return 0;
}

int link(const char *old_path, const char *new_path) {
    if (sys_link(old_path, new_path) != 0) {
        os_stat_t st;
        if (sys_stat(old_path, &st) != 0) {
            errno = ENOENT;
        } else {
            errno = __lean_path_errno(new_path, 1);
        }
        return -1;
    }
    return 0;
}

int fsync(int fd) {
    return (int)sys_fsync(fd);
}

int fdatasync(int fd) {
    return fsync(fd);
}

long readlink(const char *path, char *buffer, size_t bufsiz) {
    return sys_readlink(path, buffer, bufsiz);
}

uid_t getuid(void) {
    return 0;
}

uid_t geteuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

char *getlogin(void) {
    static char name[] = "root";
    return name;
}

int setuid(uid_t uid) {
    if (uid == 0) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int setgid(gid_t gid) {
    if (gid == 0) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int chown(const char *path, uid_t uid, gid_t gid) {
    (void)path;
    (void)uid;
    (void)gid;
    errno = EPERM;
    return -1;
}

int fchown(int fd, uid_t uid, gid_t gid) {
    (void)fd;
    (void)uid;
    (void)gid;
    errno = EPERM;
    return -1;
}

int lchown(const char *path, uid_t uid, gid_t gid) {
    (void)path;
    (void)uid;
    (void)gid;
    errno = EPERM;
    return -1;
}

int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags) {
    (void)dirfd;
    (void)path;
    (void)uid;
    (void)gid;
    (void)flags;
    errno = EPERM;
    return -1;
}

pid_t getppid(void) {
    return (pid_t)sys_getppid();
}

int getpagesize(void) {
    return 4096;
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:
        return 4096;
    case _SC_OPEN_MAX:
        return 128;
    case _SC_CLK_TCK:
        return 100;
    case _SC_NPROCESSORS_ONLN:
        return -1;
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES: {
        os_meminfo_t mi;
        if (sys_meminfo(&mi) != 0) {
            return -1;
        }
        return (long)(name == _SC_PHYS_PAGES ? mi.total_frames : mi.free_frames);
    }

    case _SC_FSYNC:
    case _SC_MEMORY_PROTECTION:
    case _SC_MONOTONIC_CLOCK:
    case _SC_THREAD_SAFE_FUNCTIONS:
    case _SC_THREADS:
    case _SC_THREAD_ATTR_STACKSIZE:
    case _SC_VERSION:
    case _SC_2_C_BIND:
    case _SC_2_VERSION:
        return 200809L;
    case _SC_MAPPED_FILES:
        return 200809L;
    case _SC_JOB_CONTROL:
    case _SC_REGEXP:
    case _SC_SHELL:
    case _SC_2_CHAR_TERM:
    case _SC_V7_LP64_OFF64:
    case _SC_V6_LP64_OFF64:
        return 1;

    case _SC_ADVISORY_INFO:
    case _SC_BARRIERS:
    case _SC_ASYNCHRONOUS_IO:
    case _SC_CLOCK_SELECTION:
    case _SC_CPUTIME:
    case _SC_IPV6:
    case _SC_MEMLOCK:
    case _SC_MEMLOCK_RANGE:
    case _SC_MESSAGE_PASSING:
    case _SC_PRIORITY_SCHEDULING:
    case _SC_RAW_SOCKETS:
    case _SC_READER_WRITER_LOCKS:
    case _SC_REALTIME_SIGNALS:
    case _SC_SAVED_IDS:
    case _SC_SEMAPHORES:
    case _SC_SHARED_MEMORY_OBJECTS:
    case _SC_SPAWN:
    case _SC_SPIN_LOCKS:
    case _SC_SPORADIC_SERVER:
    case _SC_SS_REPL_MAX:
    case _SC_SYNCHRONIZED_IO:
    case _SC_THREAD_ATTR_STACKADDR:
    case _SC_THREAD_CPUTIME:
    case _SC_THREAD_PRIO_INHERIT:
    case _SC_THREAD_PRIO_PROTECT:
    case _SC_THREAD_PRIORITY_SCHEDULING:
    case _SC_THREAD_PROCESS_SHARED:
    case _SC_THREAD_ROBUST_PRIO_INHERIT:
    case _SC_THREAD_ROBUST_PRIO_PROTECT:
    case _SC_THREAD_SPORADIC_SERVER:
    case _SC_TIMEOUTS:
    case _SC_TIMERS:
    case _SC_TRACE:
    case _SC_TRACE_EVENT_FILTER:
    case _SC_TRACE_EVENT_NAME_MAX:
    case _SC_TRACE_INHERIT:
    case _SC_TRACE_LOG:
    case _SC_TRACE_NAME_MAX:
    case _SC_TRACE_SYS_MAX:
    case _SC_TRACE_USER_EVENT_MAX:
    case _SC_TYPED_MEMORY_OBJECTS:
    case _SC_V7_ILP32_OFF32:
    case _SC_V7_ILP32_OFFBIG:
    case _SC_V7_LPBIG_OFFBIG:
    case _SC_V6_ILP32_OFF32:
    case _SC_V6_ILP32_OFFBIG:
    case _SC_V6_LPBIG_OFFBIG:
    case _SC_2_C_DEV:
    case _SC_2_FORT_DEV:
    case _SC_2_FORT_RUN:
    case _SC_2_LOCALEDEF:
    case _SC_2_PBS:
    case _SC_2_PBS_ACCOUNTING:
    case _SC_2_PBS_CHECKPOINT:
    case _SC_2_PBS_LOCATE:
    case _SC_2_PBS_MESSAGE:
    case _SC_2_PBS_TRACK:
    case _SC_2_SW_DEV:
    case _SC_2_UPE:
    case _SC_XOPEN_CRYPT:
    case _SC_XOPEN_ENH_I18N:
    case _SC_XOPEN_REALTIME:
    case _SC_XOPEN_REALTIME_THREADS:
    case _SC_XOPEN_SHM:
    case _SC_XOPEN_STREAMS:
    case _SC_XOPEN_UNIX:
    case _SC_XOPEN_UUCP:
    case _SC_XOPEN_VERSION:
    case _SC_AIO_LISTIO_MAX:
    case _SC_AIO_MAX:
    case _SC_AIO_PRIO_DELTA_MAX:
    case _SC_DELAYTIMER_MAX:
    case _SC_MQ_OPEN_MAX:
    case _SC_MQ_PRIO_MAX:
    case _SC_RTSIG_MAX:
    case _SC_SEM_NSEMS_MAX:
    case _SC_SEM_VALUE_MAX:
    case _SC_SIGQUEUE_MAX:
    case _SC_TIMER_MAX:
    case _SC_BC_BASE_MAX:
    case _SC_BC_DIM_MAX:
    case _SC_BC_SCALE_MAX:
    case _SC_BC_STRING_MAX:
    case _SC_ATEXIT_MAX:
        return -1;

    case _SC_ARG_MAX:
        return 32 * 4096;
    case _SC_CHILD_MAX:
        return 128;
    case _SC_COLL_WEIGHTS_MAX:
        return 0;
    case _SC_EXPR_NEST_MAX:
        return 32;
    case _SC_HOST_NAME_MAX:
        return 64;
    case _SC_IOV_MAX:
    case _SC_UIO_MAXIOV:
        return 16;
    case _SC_LINE_MAX:
        return 4096;
    case _SC_LOGIN_NAME_MAX:
        return 32;
    case _SC_NGROUPS_MAX:
        return 1;
    case _SC_NPROCESSORS_CONF:
        return -1;
    case _SC_RE_DUP_MAX:
        return 255;
    case _SC_STREAM_MAX:
        return 16;
    case _SC_SYMLOOP_MAX:
        return 8;
    case _SC_TTY_NAME_MAX:
        return 32;
    case _SC_TZNAME_MAX:
        return 3;
    case _SC_THREAD_DESTRUCTOR_ITERATIONS:
        return 4;
    case _SC_THREAD_KEYS_MAX:
        return 32;
    case _SC_THREAD_STACK_MIN:
        return 4096;
    case _SC_THREAD_THREADS_MAX:
        return 128;
    case _SC_GETPW_R_SIZE_MAX:
    case _SC_GETGR_R_SIZE_MAX:
        return 256;
    default:
        return -1;
    }
}

size_t confstr(int name, char *buffer, size_t length) {
    const char *value;
    switch (name) {
    case _CS_PATH:
        value = "/bin";
        break;
    case _CS_V7_ENV:
    case _CS_V6_ENV:
        value = "";
        break;
    default:
        errno = EINVAL;
        return 0;
    }
    size_t n = 0;
    while (value[n]) {
        n++;
    }
    if (buffer && length > 0) {
        size_t copy = (n + 1 <= length) ? n + 1 : length;
        for (size_t i = 0; i + 1 < copy; i++) {
            buffer[i] = value[i];
        }
        buffer[copy - 1] = '\0';
    }
    return n + 1;
}

pid_t setsid(void) {
    return (pid_t)sys_setsid();
}

pid_t getsid(pid_t pid) {
    return (pid_t)sys_getsid(pid);
}

int setpgid(pid_t pid, pid_t pgid) {
    return (int)sys_setpgid(pid, pgid);
}

pid_t getpgid(pid_t pid) {
    return (pid_t)sys_getpgid(pid);
}

pid_t getpgrp(void) {
    return (pid_t)sys_getpgid(0);
}

int setpgrp(void) {
    return (int)sys_setpgid(0, 0);
}

int dup(int oldfd) {
    if (oldfd < 0) {
        errno = EBADF;
        return -1;
    }
    for (int i = 0; i < OPEN_MAX; i++) {
        if (i == oldfd) {
            continue;
        }
        if (sys_fcntl(i, F_GETFD_COMMAND, 0) < 0) {
            return dup2(oldfd, i);
        }
    }
    errno = EMFILE;
    return -1;
}

int fchdir(int fd) {
    char path[PATH_MAX];
    if (sys_fdpath(fd, path, sizeof(path)) < 0) {
        errno = EBADF;
        return -1;
    }
    return chdir(path);
}

int chroot(const char *path) {
    (void)path;
    errno = EPERM;
    return -1;
}

int gethostname(char *name, size_t length) {
    static const char host[] = "lean_os";
    if (!name || length == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t n = sizeof(host) - 1;
    if (length < n + 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t i = 0; i <= n; i++) {
        name[i] = host[i];
    }
    return 0;
}

int sethostname(const char *name, size_t length) {
    (void)name;
    (void)length;
    errno = EPERM;
    return -1;
}

pid_t vfork(void) {
    return fork();
}

void sync(void) {
    sys_sync();
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)sys_alarm(seconds);
}

static long pathconf_value(int name) {
    switch (name) {
    case _PC_LINK_MAX:
        return 65535;
    case _PC_NAME_MAX:
        return NAME_MAX;
    case _PC_PATH_MAX:
        return PATH_MAX;
    case _PC_PIPE_BUF:
        return 4096;
    case _PC_CHOWN_RESTRICTED:
        return 1;
    case _PC_NO_TRUNC:
        return 1;
    case _PC_SYMLINK_MAX:
        return PATH_MAX;
    case _PC_FILESIZEBITS:
        return 33;
    case _PC_MAX_CANON:
    case _PC_MAX_INPUT:
        return 256;
    case _PC_VDISABLE:
        return 0;
    case _PC_ASYNC_IO:
    case _PC_PRIO_IO:
    case _PC_SYNC_IO:
        return -1;
    default:
        errno = EINVAL;
        return -1;
    }
}

long pathconf(const char *path, int name) {
    os_stat_t st;
    if (!path || sys_stat(path, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    return pathconf_value(name);
}

long fpathconf(int fd, int name) {
    os_stat_t st;
    if (fd < 0 || sys_fstat(fd, &st) != 0) {
        errno = EBADF;
        return -1;
    }
    return pathconf_value(name);
}

int ttyname_r(int fd, char *buffer, size_t length) {
    static const char name[] = "/dev/tty";
    if (!buffer) {
        return EINVAL;
    }
    if (!isatty(fd)) {
        return ENOTTY;
    }
    if (length < sizeof(name)) {
        return ERANGE;
    }
    for (size_t i = 0; i < sizeof(name); i++) {
        buffer[i] = name[i];
    }
    return 0;
}

char *ttyname(int fd) {
    static char shared[16];
    return ttyname_r(fd, shared, sizeof(shared)) == 0 ? shared : (char *)0;
}

ssize_t getrandom(void *buffer, size_t length, unsigned int flags) {
    if (flags & ~(unsigned int)(GRND_NONBLOCK | GRND_RANDOM)) {
        errno = EINVAL;
        return -1;
    }
    long r = sys_getrandom(buffer, length, flags);
    if (r < 0) {
        errno = EFAULT;
        return -1;
    }
    return (ssize_t)r;
}

int getentropy(void *buffer, size_t length) {
    if (length > 256) {
        errno = EIO;
        return -1;
    }
    return sys_getrandom(buffer, length, 0) == (long)length ? 0 : -1;
}
