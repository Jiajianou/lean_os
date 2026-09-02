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
#include <stdlib.h>   /* M84: getenv, for execvp's PATH search */
#include <sys/wait.h> /* M84: waitpid */
#include "paths.h"    /* system_api/include/paths.h - PATH_MAX_LEN */
#include <errno.h>    /* M89: fcntl's lock commands refuse with EOPNOTSUPP */
#include <limits.h>   /* M89: OPEN_MAX, the range dup() searches */
#include "proc.h"     /* system_api/include/proc.h - os_meminfo_t, M89 */
#include <dirent.h>   /* M89: NAME_MAX, which pathconf reports */
#include <string.h>   /* M89: memset, for wait4's usage on failure */
#include <sys/resource.h> /* M89: getrusage, which wait4 reports from */

/* ---- M89: an errno for a kernel that only says -1 ---------------------
 *
 * Every path syscall in this OS reports failure as -1 and nothing else.
 * That was survivable while every caller was in this tree - a program
 * that could not create a file printed its own message and stopped. It
 * is not survivable for a ported program, and toybox proved it on the
 * first run: `mkdir -p a/b/c` creates each component and *tolerates
 * EEXIST*, so a mkdir that fails without saying why makes `mkdir -p`
 * fail on a directory that already exists. `rm -f` reads errno the same
 * way, and so does every "create it if it isn't there" in the world.
 *
 * So this infers the errno from questions the caller can still ask, and
 * the inference is deliberately narrow: does the path exist, does its
 * parent exist, is the parent a directory. Those three answers cover
 * every case a program actually branches on.
 *
 * **It is an inference and not a report, and the difference is real.**
 * The state is re-examined after the failure rather than at it, so a
 * path that appeared between the two is described as it is now. Nothing
 * here is racing anything today - one principal, and the programs that
 * do this are sequential - and the honest fix is a kernel that returns
 * the reason, which is a change to every path syscall's ABI and is not
 * this milestone. Written down so the next person meets it here.
 */
int __lean_path_errno(const char *path, int creating) {
    if (!path || !path[0]) {
        return EFAULT;
    }
    os_stat_t st;
    if (sys_stat(path, &st) == 0) {
        /* It is there. For an operation that was trying to make it, that
         * is exactly EEXIST; for anything else, the refusal came from
         * somewhere this cannot see - most often a capability the caller
         * does not hold (M65), which is what EACCES means. */
        return creating ? EEXIST : EACCES;
    }
    /* Not there. Split "no such file" from "no such directory to put it
     * in", which is the distinction a program creating a path acts on. */
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
    return st.is_dir ? ENOENT : ENOTDIR;
}

int chdir(const char *path) {
    if (sys_chdir(path) != 0) {
        os_stat_t st;
        if (sys_stat(path, &st) == 0 && !st.is_dir) {
            errno = ENOTDIR;
        } else {
            errno = __lean_path_errno(path, 0);
        }
        return -1;
    }
    return 0;
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

void _exit(int status) {
    /* Deliberately not a call to exit(): see the header. They do the
     * same thing today and must not be the same name. */
    sys_exit(status);
    for (;;) {
    }
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
    if (sys_rmdir(path) != 0) {
        os_stat_t st;
        if (sys_stat(path, &st) == 0) {
            /* It is there and it would not go. Two reasons this kernel
             * has: it is not a directory, or it is not empty. */
            errno = st.is_dir ? ENOTEMPTY : ENOTDIR;
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
        if (sys_stat(path, &st) == 0 && st.is_dir) {
            errno = EISDIR; /* SYS_unlink refuses a directory - rmdir is its call */
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
    int fd = (int)sys_open(path, (uint32_t)flags);
    if (fd < 0) {
        /* O_EXCL|O_CREAT failing on a file that exists is EEXIST, which
         * is the whole point of that flag pair and the one errno a
         * program using it as a lock reads. Everything else falls to the
         * general inference. */
        errno = __lean_path_errno(path, (flags & O_CREAT) != 0);
    }
    return fd;
}

/* See <fcntl.h> for why this answers what it answers. */
int fcntl(int fd, int cmd, ...) {
    if (fd < 0) {
        return -1;
    }
    switch (cmd) {
    /* M84: FD_CLOEXEC is a real flag now, and these two are real calls.
     * There is an exec on this machine, so "close this when a different
     * program starts" finally means something - see SYS_fcntl. */
    case F_GETFD:
        return (int)sys_fcntl(fd, F_GETFD_CMD, 0);
    case F_SETFD: {
        __builtin_va_list ap;
        __builtin_va_start(ap, cmd);
        int arg = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        return (int)sys_fcntl(fd, F_SETFD_CMD, arg & FD_CLOEXEC);
    }
    /* F_GETFL/F_SETFL are unchanged and still honest. They are about
     * O_NONBLOCK, every descriptor here is blocking, and a silent success
     * would be a program believing otherwise. M88 is where that changes. */
    case F_GETFL:
        return 0;
    case F_SETFL: {
        __builtin_va_list ap;
        __builtin_va_start(ap, cmd);
        int arg = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        return arg == 0 ? 0 : -1;
    }
    /* M89: record locks, declared so a program compiles and refused so
     * that one never believes it holds a lock nothing is keeping. See
     * <fcntl.h> for the argument, which is M65's applied to a file. */
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        errno = EOPNOTSUPP;
        return -1;
    default:
        return -1;
    }
}

pid_t fork(void) {
    return (pid_t)sys_fork();
}

/* M84: replaces this program with another. Does not return on success -
 * which is why every caller in the world writes `execve(...); perror(...)`
 * with no `if` around it. */
int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)sys_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

/* M84: PATH search, which M75 deferred with "deliberately not a full
 * PATH-search exec... M72 already owns those". M72 owns the *shell's*
 * copy of it; this is the libc's, and the difference matters because a
 * program that calls execvp is not going through a shell.
 *
 * A name containing a '/' is a path and is used as given - the same rule
 * every execvp follows, and the reason `./a.out` works. Otherwise each
 * PATH element is tried in order. An empty or unset PATH means only the
 * current directory, which is what the standard says and not what most
 * people expect; it is left as the standard has it rather than improved.
 */
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
        /* M89: the default is /bin, not the current directory.
         *
         * The note that stood here said an unset PATH means "only the
         * current directory, which is what the standard says and not
         * what most people expect; it is left as the standard has it
         * rather than improved." That was a misreading. POSIX says an
         * unset PATH uses an *implementation-defined* default that finds
         * the standard utilities, which is exactly what confstr(_CS_PATH)
         * reports - and on this machine that is "/bin".
         *
         * It matters rather than being pedantry: `xargs` exec's `grep`
         * by name, and a process spawned with no environment (which is
         * every process the kernel starts) would search "." and not find
         * it. Found by M89's own pipeline, in its middle stage. */
        path = "/bin";
    }
    char attempt[PATH_MAX_LEN];
    const char *p = path;
    while (*p) {
        size_t n = 0;
        while (*p && *p != ':' && n < sizeof(attempt) - 2) {
            attempt[n++] = *p++;
        }
        while (*p && *p != ':') {
            p++; /* an element too long to try is skipped, not truncated */
        }
        if (n == 0) {
            attempt[n++] = '.'; /* an empty element means the current directory */
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
            /* Only attempted if the whole name fitted - a truncated name
             * is a different program. */
            execve(attempt, argv, environ);
        }
        if (*p == ':') {
            p++;
        }
    }
    return -1; /* nothing on PATH was runnable */
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)sys_waitpid(pid, status, options);
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

/* M89: see <sys/wait.h> for what `usage` is and is not. */
pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage) {
    pid_t r = waitpid(pid, status, options);
    if (usage) {
        /* Read after the reap, so that the child whose exit this call
         * just observed is already counted - the accounting happens at
         * reap (see SYS_rusage), and asking first would report the
         * total from before it. */
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
            errno = ENOENT; /* the thing being linked to is not there */
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

long readlink(const char *path, char *buf, size_t bufsiz) {
    return sys_readlink(path, buf, bufsiz);
}

/* ---- M88: identity, and why 0 is the truth here ----------------------
 *
 * See <unistd.h> for the argument. In short: M65 refused to invent a
 * user, and a machine with exactly one principal that reports one
 * principal is not inventing anything. The real and effective forms are
 * equal because there is no setuid here for them to differ about.
 */
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

/* M89: the setters, and the ownership refusals. See <unistd.h> and
 * <sys/stat.h> for the argument; the short version is that there is one
 * principal and leanfs stores no owner, so becoming somebody else and
 * giving a file away are both operations with nothing behind them. */
int setuid(uid_t uid) {
    if (uid == 0) {
        return 0; /* already are - a success that changes nothing, truthfully */
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

/* Each of these is a fact this machine can actually state. The ones it
 * cannot are -1, which is what sysconf means by "no limit is defined" -
 * and is a far better answer to a configure script than a plausible
 * number it would then build against. */
long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:
        return 4096; /* the only page size this kernel maps */
    case _SC_OPEN_MAX:
        return 128;  /* MAX_FDS in kernel/sched/sched.h */
    case _SC_CLK_TCK:
        return 100;  /* PIT_HZ - the tick this machine actually counts in */
    case _SC_NPROCESSORS_ONLN:
        /* Not answerable from user space: SYS_taskinfo reports tasks and
         * SYS_idle_ticks takes a cpu index without saying how many there
         * are. Rather than probing idle_ticks until it fails - which
         * would be inferring a number from an error - this says it does
         * not know. */
        return -1;
    /* M89: real numbers. This said "-1 is the honest answer until
     * something asks" for one milestone, and `free` asked - so
     * SYS_meminfo exists and these report what the PMM counts rather
     * than what /proc renders, which is also what keeps sysconf from
     * depending on a filesystem being mounted. */
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES: {
        os_meminfo_t mi;
        if (sys_meminfo(&mi) != 0) {
            return -1;
        }
        return (long)(name == _SC_PHYS_PAGES ? mi.total_frames : mi.free_frames);
    }

    /* ---- M89: the POSIX options ------------------------------------
     *
     * -1 means "this system does not have that option", which is what
     * POSIX says it means and is an answer rather than an error. See
     * <unistd.h> for why this list is the useful part of that header,
     * and for which of these are expected to change as later milestones
     * land. The cases are grouped by their answer rather than
     * alphabetically, so that "what does this machine have" is readable
     * in one pass. */
    case _SC_FSYNC:                 /* fsync/fdatasync, M93 */
    case _SC_MEMORY_PROTECTION:     /* mprotect and a real NX bit, M91 */
    case _SC_MONOTONIC_CLOCK:       /* SYS_uptime_ms, which cannot go back */
    case _SC_THREAD_SAFE_FUNCTIONS: /* the _r forms, M89 */
    case _SC_THREADS:               /* M79 */
    case _SC_THREAD_ATTR_STACKSIZE: /* pthread_attr_setstacksize, M79 */
    case _SC_VERSION:
    case _SC_2_C_BIND:
    case _SC_2_VERSION:
        return 200809L;
    case _SC_JOB_CONTROL:           /* M85 */
    case _SC_REGEXP:                /* M89's engine, graded against the host's */
    case _SC_SHELL:                 /* M72/M86 */
    case _SC_2_CHAR_TERM:           /* M85's line discipline */
    case _SC_V7_LP64_OFF64:         /* the only model this OS has */
    case _SC_V6_LP64_OFF64:
        return 1;

    /* Not here, each for a reason this file has already written down
     * somewhere: no aio, no message queues, no realtime signals or
     * timers, no scheduling policy a process can set (M69 decided that),
     * no second principal to have saved ids (M65), no IPv6 or raw
     * sockets, no swap to lock pages against, no tracing, and no
     * barriers/rwlocks/spinlocks/semaphores in <pthread.h> - which are
     * M96's, over a futex that does not exist yet.
     *
     * _SC_MAPPED_FILES is the one to watch: file-backed mmap is M91's
     * unfinished half, and this line is what turns into 200809L on the
     * day it lands. */
    case _SC_ADVISORY_INFO:
    case _SC_BARRIERS:
    case _SC_ASYNCHRONOUS_IO:
    case _SC_CLOCK_SELECTION:
    case _SC_CPUTIME:
    case _SC_IPV6:
    case _SC_MAPPED_FILES:
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
    case _SC_2_C_DEV:      /* M98 is where a compiler arrives */
    case _SC_2_FORT_DEV:
    case _SC_2_FORT_RUN:
    case _SC_2_LOCALEDEF:  /* one locale, and it is not definable */
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
    case _SC_BC_BASE_MAX:  /* there is no bc here */
    case _SC_BC_DIM_MAX:
    case _SC_BC_SCALE_MAX:
    case _SC_BC_STRING_MAX:
    case _SC_ATEXIT_MAX:   /* there is no atexit - see <unistd.h>'s _exit */
        return -1;

    /* ---- the sizes, every one of them a number this machine keeps --- */
    case _SC_ARG_MAX:
        /* USER_ARG_BYTES: the region SYS_spawn copies the whole vector
         * into, argc and both pointer arrays included. M89 raised it
         * from one page to thirty-two because `xargs` sizes its batches
         * from this number and refused to run at all below about 8 KiB -
         * see USER_ARG_PAGES for the measurement and for why the pages
         * are not actually allocated until they are used. */
        return 32 * 4096;
    case _SC_CHILD_MAX:
        return 128; /* MAX_TASKS in kernel/sched/sched.h */
    case _SC_COLL_WEIGHTS_MAX:
        return 0;   /* the C locale collates by byte and weighs nothing */
    case _SC_EXPR_NEST_MAX:
        return 32;
    case _SC_HOST_NAME_MAX:
        return 64;
    case _SC_IOV_MAX:
    case _SC_UIO_MAXIOV:
        /* readv/writev loop over the vector calling read and write (see
         * <sys/uio.h>), so there is no kernel limit to report - this is
         * the number POSIX requires at minimum, which is the honest
         * answer for "no limit of its own". */
        return 16;
    case _SC_LINE_MAX:
        return 4096;
    case _SC_LOGIN_NAME_MAX:
        return 32;
    case _SC_NGROUPS_MAX:
        return 1;   /* one principal, one group - see <grp.h> */
    case _SC_NPROCESSORS_CONF:
        return -1;  /* same as _SC_NPROCESSORS_ONLN above, same reason */
    case _SC_RE_DUP_MAX:
        return 255; /* the interval bound M89's regex engine accepts */
    case _SC_STREAM_MAX:
        return 16;  /* FOPEN_MAX in <stdio.h> */
    case _SC_SYMLOOP_MAX:
        return 8;   /* how many links leanfs follows before ELOOP */
    case _SC_TTY_NAME_MAX:
        return 32;
    case _SC_TZNAME_MAX:
        return 3;   /* "UTC", and there is no other - see <time.h> */
    case _SC_THREAD_DESTRUCTOR_ITERATIONS:
        return 4;
    case _SC_THREAD_KEYS_MAX:
        return 32;  /* PTHREAD_KEYS_MAX in <pthread.h> */
    case _SC_THREAD_STACK_MIN:
        return 4096;
    case _SC_THREAD_THREADS_MAX:
        return 128; /* MAX_TASKS again: a thread is a task here (M79) */
    default:
        return -1;
    }
}

/* M89: see <unistd.h>. Two strings, and both are short enough that the
 * "how long is it" call and the "give it to me" call share one path. */
size_t confstr(int name, char *buf, size_t len) {
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
    if (buf && len > 0) {
        size_t copy = (n + 1 <= len) ? n + 1 : len;
        for (size_t i = 0; i + 1 < copy; i++) {
            buf[i] = value[i];
        }
        buf[copy - 1] = '\0';
    }
    return n + 1; /* the length INCLUDING the NUL, which is confstr's contract */
}

/* ---- M89: the names a ported program calls ---------------------------
 *
 * Every one of these is a thin renaming of something that already
 * existed, or a refusal with a reason. See <unistd.h> for which is
 * which; the ones with anything to explain explain it there.
 */
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
    /* The lowest free descriptor, found by asking each one whether it is
     * open. fcntl(F_GETFD) on a closed descriptor fails, which is the
     * only "is this fd in use" question this kernel answers - and it has
     * to be asked, because dup2 onto a live descriptor closes it. */
    for (int i = 0; i < OPEN_MAX; i++) {
        if (i == oldfd) {
            continue;
        }
        if (sys_fcntl(i, F_GETFD_CMD, 0) < 0) {
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

int gethostname(char *name, size_t len) {
    /* There is no hostname on this machine: nothing stores one, DHCP
     * (M64) does not ask for one, and the DNS client resolves names
     * without ever needing to state its own. "lean_os" is what
     * <sys/utsname.h>'s nodename already reports, so this is the same
     * fact in the spelling a different program asks for - not a second
     * invented answer. */
    static const char host[] = "lean_os";
    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t n = sizeof(host) - 1;
    if (len < n + 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t i = 0; i <= n; i++) {
        name[i] = host[i];
    }
    return 0;
}

int sethostname(const char *name, size_t len) {
    (void)name;
    (void)len;
    /* Refused rather than stored: a hostname this libc remembered would
     * be forgotten at exec and invisible to every other process, which
     * is not what a program setting one is asking for. */
    errno = EPERM;
    return -1;
}

pid_t vfork(void) {
    return fork(); /* see <unistd.h> - M83's fork is already copy-on-write */
}

void sync(void) {
    sys_sync();
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)sys_alarm(seconds);
}

/* ---- M89: pathconf/fpathconf - see <unistd.h> ------------------------ */

static long pathconf_value(int name) {
    switch (name) {
    case _PC_LINK_MAX:
        /* leanfs's own limit on names pointing at one inode (M93). */
        return 65535;
    case _PC_NAME_MAX:
        return NAME_MAX;
    case _PC_PATH_MAX:
        return PATH_MAX;
    case _PC_PIPE_BUF:
        /* What a write of this size or less is guaranteed atomic at -
         * kernel/ipc/pipe.h's buffer, which is one page. */
        return 4096;
    case _PC_CHOWN_RESTRICTED:
        /* 1 means "only a privileged process may give a file away". Here
         * nobody may: chown refuses for everyone (see <unistd.h>), which
         * is the restricted case taken to its end. */
        return 1;
    case _PC_NO_TRUNC:
        /* 1 means an over-long name is an error rather than being
         * silently shortened, which is what leanfs does. */
        return 1;
    case _PC_SYMLINK_MAX:
        return PATH_MAX;
    case _PC_FILESIZEBITS:
        /* leanfs sizes are 32-bit (M81: files up to 4 GiB), so 33 bits
         * is what it takes to hold the largest size plus its sign. */
        return 33;
    case _PC_MAX_CANON:
    case _PC_MAX_INPUT:
        /* The line discipline's input buffer - kernel/dev/tty.c. */
        return 256;
    case _PC_VDISABLE:
        /* The c_cc value that disables a control character. 0 is what
         * every terminal uses and what this discipline treats as
         * "never matches", since no key sends a NUL. */
        return 0;
    case _PC_ASYNC_IO:
    case _PC_PRIO_IO:
    case _PC_SYNC_IO:
        /* -1: no aio, no prioritized I/O, no O_SYNC - the same three
         * answers sysconf gives for the same three options. */
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

/* M89: see <unistd.h>. One terminal, and it is /dev/tty. */
int ttyname_r(int fd, char *buf, size_t len) {
    static const char name[] = "/dev/tty";
    if (!buf) {
        return EINVAL;
    }
    if (!isatty(fd)) {
        return ENOTTY;
    }
    if (len < sizeof(name)) {
        return ERANGE;
    }
    for (size_t i = 0; i < sizeof(name); i++) {
        buf[i] = name[i];
    }
    return 0;
}

char *ttyname(int fd) {
    static char shared[16];
    return ttyname_r(fd, shared, sizeof(shared)) == 0 ? shared : (char *)0;
}
