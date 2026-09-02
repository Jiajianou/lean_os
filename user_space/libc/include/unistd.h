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
/* M89: which errno a failed path operation should report on a machine
 * whose path syscalls all return -1 and nothing else. Not POSIX and not
 * for programs - this library's own path calls use it, and it is
 * declared here rather than being static so that the several files that
 * need it share one inference. See unistd.c for what it can and cannot
 * tell, which is the part worth reading. */
int __lean_path_errno(const char *path, int creating);

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

/* M86: exit without running anything on the way out.
 *
 * The shell asked for this by name and the reason is fork: a child that
 * has decided not to exec must leave WITHOUT running what the parent
 * registered on its way out. Nothing here registers anything yet -
 * `exit` is a syscall and no more (see stdlib.c) - so today the two are
 * the same call, and that is worth stating rather than hiding. The
 * distinction becomes real the day this libc grows atexit or a buffered
 * stdio that flushes, and a shell that had spelled it `exit` would then
 * quietly flush its parent's buffers once per forked command. */
void _exit(int status) __attribute__((noreturn));
/* M80 groundwork. `isatty` answers from what this system actually knows:
 * fd 0 and 1 are the implicit stdin/stdout every task starts with
 * (kernel/sched/sched.h's fd table), and everything else is a pipe, a
 * socket or a file. There is no terminal device here to ask, so this is
 * the honest approximation and not a stub - it is right for every use a
 * ported program puts it to (deciding whether to prompt). */
int isatty(int fd);
/* M89: the name of the terminal on `fd`, or NULL if it is not one.
 *
 * There is exactly one terminal on this machine and its path is
 * "/dev/tty" (M87's devfs), so this answers that for anything isatty
 * says yes to and NULL otherwise. That is not a stub: a system with one
 * terminal reporting one terminal is the same shape of truth <pwd.h>
 * documents for its one user. The _r form takes the caller's buffer. */
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t len);
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

/* M87: symbolic links. Note symlink's argument order - the target first,
 * then the name to create - which is symlink(2)'s everywhere and the
 * reverse of what most people guess.
 *
 * `readlink` does NOT follow the link it is given, which is the whole
 * point of it, and does NOT NUL-terminate: it returns the byte count,
 * exactly as POSIX specifies, because a target may legitimately contain
 * anything a path can. Every caller has to terminate it themselves and
 * every caller written elsewhere already does. */
/* ---- M88: who is running this, and how much of what there is --------
 *
 * `getuid` and friends return 0, and that is not a placeholder. M65
 * argued at length that there are no users on this machine and refused
 * to invent one; a machine with exactly one principal that reports one
 * principal is telling the truth. What M65 declined was a *permission
 * model* that pretended to enforce something, and nothing here enforces
 * anything - `access()` still says so in its own comment, and `chmod`
 * is still a truthful failure.
 *
 * The effective and real forms are the same number for the same reason:
 * there is no setuid on this machine, so there is nothing for them to
 * differ about. A program that compares them is asking "am I running
 * with borrowed authority", and the honest answer here is no. */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

/* M89: the ownership setters, refused for the reason <sys/stat.h> gives
 * for chmod. leanfs stores no owner; a call that returned 0 would be
 * claiming a file now belongs to somebody. */
int chown(const char *path, uid_t uid, gid_t gid);
int fchown(int fd, uid_t uid, gid_t gid);
int lchown(const char *path, uid_t uid, gid_t gid);
int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags);

/* M89: the setters for the identity calls above. There is one principal
 * and it is uid 0, so setting the id to 0 succeeds (it is already that)
 * and setting it to anything else fails - which is exactly what a
 * privileged process on any Unix sees when it tries to become a user
 * that does not exist. */
int setuid(uid_t uid);
int setgid(gid_t gid);

/* M89: the parent's pid. The kernel has held `parent_id` in every task
 * since M14 and never had a call that reported it - SYS_taskinfo's
 * whole-table snapshot needs CAP_PROC_LIST, which is exactly the wrong
 * shape for a process asking about itself. */
pid_t getppid(void);

/* M89: process groups and sessions, in the POSIX spelling. The syscalls
 * have been here since M85 (SYS_setsid, SYS_getsid, SYS_setpgid,
 * SYS_getpgid) and only the shell, which calls the wrappers directly,
 * had ever used them. A ported program calls these names. */
pid_t setsid(void);
pid_t getsid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
int setpgrp(void);

/* M89: the lowest free descriptor naming the same file. Built over
 * dup2 - which is the only duplication the kernel has - by finding a
 * free slot first, because dup2 to a descriptor already in use would
 * close it. See unistd.c for how "free" is asked. */
int dup(int oldfd);

/* M89: refused. A root directory a process cannot escape is a boundary,
 * and this machine's boundary is the capability set (M65), which a
 * chroot does not narrow. Returning 0 would tell a program it was
 * confined when it was not, which is the one failure mode a sandbox must
 * not have. */
int chroot(const char *path);

/* M89: chdir to a directory named by a descriptor. Over SYS_fdpath, like
 * the *at() family and with the same non-atomicity - see <fcntl.h>. */
int fchdir(int fd);

/* M89: the machine's name. There is no hostname stored anywhere on this
 * system - see unistd.c for what is reported and why that is a fact
 * rather than a placeholder. sethostname refuses. */
int gethostname(char *name, size_t len);
int sethostname(const char *name, size_t len);

/* M89: fork, with the promise the caller will exec or _exit immediately.
 * On this machine it IS fork - M83's is copy-on-write, so the copy a
 * vfork exists to avoid has already been avoided. Provided as a name
 * rather than as a mechanism, and that is the honest relationship: a
 * program that uses vfork for speed gets the speed from COW, and one
 * that relies on vfork's shared address space is relying on undefined
 * behaviour that this implementation does not provide. */
pid_t vfork(void);

/* ---- sysconf: what this machine has, and what it does not -----------
 *
 * M88 shipped six of these with the note that "each answer below is a
 * real fact about this machine rather than a plausible number - see the
 * implementation, where the ones that cannot be answered return -1
 * rather than a guess."
 *
 * M89 makes that list complete rather than a subset, because `getconf`
 * asks for all of it - and the result is the most useful thing in this
 * header: **a machine that says which POSIX options it implements.**
 * Every `_SC_` name below that describes an *option* answers 200809L or
 * 1 if this system has it and **-1 if it does not**, which is what -1
 * means in POSIX and is a fact rather than a failure. A program that
 * asks whether there are message queues here gets "no" instead of an
 * error it has to interpret.
 *
 * The list is worth reading as documentation: it is the honest inventory
 * of this OS's POSIX surface, and several entries are expected to change
 * from -1 to a version as later milestones land - _SC_MAPPED_FILES when
 * M91 finishes file-backed mmap, _SC_TIMERS if a timer_create ever
 * arrives, _SC_2_C_DEV when M98 puts a compiler here.
 *
 * The numbers themselves are this project's own and are NOT Linux's;
 * they never were (see <fcntl.h>'s note on the same subject). Nothing
 * passes one across an ABI boundary - sysconf takes the name a program
 * wrote, which the compiler turned into whatever number is here.
 */
#define _SC_PAGESIZE      1
#define _SC_PAGE_SIZE     _SC_PAGESIZE /* both spellings are in use */
#define _SC_OPEN_MAX      2
#define _SC_NPROCESSORS_ONLN 3
#define _SC_CLK_TCK       4
#define _SC_PHYS_PAGES    5
#define _SC_AVPHYS_PAGES  6

/* The POSIX options. */
#define _SC_ADVISORY_INFO              100
#define _SC_BARRIERS                   101
#define _SC_ASYNCHRONOUS_IO            102
#define _SC_CLOCK_SELECTION            103
#define _SC_CPUTIME                    104
#define _SC_FSYNC                      105
#define _SC_IPV6                       106
#define _SC_JOB_CONTROL                107
#define _SC_MAPPED_FILES               108
#define _SC_MEMLOCK                    109
#define _SC_MEMLOCK_RANGE              110
#define _SC_MEMORY_PROTECTION          111
#define _SC_MESSAGE_PASSING            112
#define _SC_MONOTONIC_CLOCK            113
#define _SC_PRIORITY_SCHEDULING        114
#define _SC_RAW_SOCKETS                115
#define _SC_READER_WRITER_LOCKS        116
#define _SC_REALTIME_SIGNALS           117
#define _SC_REGEXP                     118
#define _SC_SAVED_IDS                  119
#define _SC_SEMAPHORES                 120
#define _SC_SHARED_MEMORY_OBJECTS      121
#define _SC_SHELL                      122
#define _SC_SPAWN                      123
#define _SC_SPIN_LOCKS                 124
#define _SC_SPORADIC_SERVER            125
#define _SC_SS_REPL_MAX                126
#define _SC_SYNCHRONIZED_IO            127
#define _SC_THREAD_ATTR_STACKADDR      128
#define _SC_THREAD_ATTR_STACKSIZE      129
#define _SC_THREAD_CPUTIME             130
#define _SC_THREAD_PRIO_INHERIT        131
#define _SC_THREAD_PRIO_PROTECT        132
#define _SC_THREAD_PRIORITY_SCHEDULING 133
#define _SC_THREAD_PROCESS_SHARED      134
#define _SC_THREAD_ROBUST_PRIO_INHERIT 135
#define _SC_THREAD_ROBUST_PRIO_PROTECT 136
#define _SC_THREAD_SAFE_FUNCTIONS      137
#define _SC_THREAD_SPORADIC_SERVER     138
#define _SC_THREADS                    139
#define _SC_TIMEOUTS                   140
#define _SC_TIMERS                     141
#define _SC_TRACE                      142
#define _SC_TRACE_EVENT_FILTER         143
#define _SC_TRACE_EVENT_NAME_MAX       144
#define _SC_TRACE_INHERIT              145
#define _SC_TRACE_LOG                  146
#define _SC_TRACE_NAME_MAX             147
#define _SC_TRACE_SYS_MAX              148
#define _SC_TRACE_USER_EVENT_MAX       149
#define _SC_TYPED_MEMORY_OBJECTS       150
#define _SC_VERSION                    151
#define _SC_V7_ILP32_OFF32             152
#define _SC_V7_ILP32_OFFBIG            153
#define _SC_V7_LP64_OFF64              154
#define _SC_V7_LPBIG_OFFBIG            155
#define _SC_V6_ILP32_OFF32             156
#define _SC_V6_ILP32_OFFBIG            157
#define _SC_V6_LP64_OFF64              158
#define _SC_V6_LPBIG_OFFBIG            159

/* POSIX.2, which is about utilities rather than about the kernel. */
#define _SC_2_C_BIND          200
#define _SC_2_C_DEV           201
#define _SC_2_CHAR_TERM       202
#define _SC_2_FORT_DEV        203
#define _SC_2_FORT_RUN        204
#define _SC_2_LOCALEDEF       205
#define _SC_2_PBS             206
#define _SC_2_PBS_ACCOUNTING  207
#define _SC_2_PBS_CHECKPOINT  208
#define _SC_2_PBS_LOCATE      209
#define _SC_2_PBS_MESSAGE     210
#define _SC_2_PBS_TRACK       211
#define _SC_2_SW_DEV          212
#define _SC_2_UPE             213
#define _SC_2_VERSION         214

/* X/Open. None of it is here, and each is -1 for that reason. */
#define _SC_XOPEN_CRYPT            300
#define _SC_XOPEN_ENH_I18N         301
#define _SC_XOPEN_REALTIME         302
#define _SC_XOPEN_REALTIME_THREADS 303
#define _SC_XOPEN_SHM              304
#define _SC_XOPEN_STREAMS          305
#define _SC_XOPEN_UNIX             306
#define _SC_XOPEN_UUCP             307
#define _SC_XOPEN_VERSION          308

/* Sizes and counts. */
#define _SC_AIO_LISTIO_MAX    400
#define _SC_AIO_MAX           401
#define _SC_AIO_PRIO_DELTA_MAX 402
#define _SC_ARG_MAX           403
#define _SC_ATEXIT_MAX        404
#define _SC_BC_BASE_MAX       405
#define _SC_BC_DIM_MAX        406
#define _SC_BC_SCALE_MAX      407
#define _SC_BC_STRING_MAX     408
#define _SC_CHILD_MAX         409
#define _SC_COLL_WEIGHTS_MAX  410
#define _SC_DELAYTIMER_MAX    411
#define _SC_EXPR_NEST_MAX     412
#define _SC_HOST_NAME_MAX     413
#define _SC_IOV_MAX           414
#define _SC_LINE_MAX          415
#define _SC_LOGIN_NAME_MAX    416
#define _SC_NGROUPS_MAX       417
#define _SC_MQ_OPEN_MAX       418
#define _SC_MQ_PRIO_MAX       419
#define _SC_NPROCESSORS_CONF  420
#define _SC_RE_DUP_MAX        421
#define _SC_RTSIG_MAX         422
#define _SC_SEM_NSEMS_MAX     423
#define _SC_SEM_VALUE_MAX     424
#define _SC_SIGQUEUE_MAX      425
#define _SC_STREAM_MAX        426
#define _SC_SYMLOOP_MAX       427
#define _SC_TIMER_MAX         428
#define _SC_TTY_NAME_MAX      429
#define _SC_TZNAME_MAX        430
#define _SC_UIO_MAXIOV        431
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 432
#define _SC_THREAD_KEYS_MAX   433
#define _SC_THREAD_STACK_MIN  434
#define _SC_THREAD_THREADS_MAX 435

long sysconf(int name);

/* ---- confstr: the two strings a system has ---------------------------
 *
 * `_CS_PATH` is the PATH a program can rely on to find the standard
 * utilities, which here is "/bin" and nothing else - there is one
 * directory of programs and it is that one. `_CS_V7_ENV` is empty,
 * because there are no environment settings needed to get a conforming
 * environment: this system's only environment is a conforming one, so
 * far as it conforms at all.
 *
 * Same contract as every confstr: returns the length including the NUL,
 * copies what fits, and a `len` of 0 asks only for the length. */
#define _CS_PATH    1
#define _CS_V7_ENV  2
#define _CS_V6_ENV  3
size_t confstr(int name, char *buf, size_t len);

/* M89: the per-path limits, which `stat -f` asks for by name.
 *
 * Every answer is a constant here rather than a per-path lookup, and
 * that is a fact about this machine rather than a shortcut: there is one
 * filesystem (see /proc/mounts), so a limit cannot differ between two
 * paths. `path` and `fd` are validated - a nonexistent path is still an
 * error, because a program probing a path it cannot reach should hear
 * about that rather than get a number. */
#define _PC_LINK_MAX       1
#define _PC_NAME_MAX       2
#define _PC_PATH_MAX       3
#define _PC_PIPE_BUF       4
#define _PC_CHOWN_RESTRICTED 5
#define _PC_NO_TRUNC       6
#define _PC_SYMLINK_MAX    7
#define _PC_ASYNC_IO       8
#define _PC_PRIO_IO        9
#define _PC_SYNC_IO        10
#define _PC_FILESIZEBITS   11
#define _PC_MAX_CANON      12
#define _PC_MAX_INPUT      13
#define _PC_VDISABLE       14
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);

/* POSIX spells the page size both ways and programs use both. */
int getpagesize(void);

/* M89: <unistd.h> is where a program looks for these two; the
 * implementation and the resolution argument are in <time.h> beside
 * nanosleep, which is what all three are. */
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

/* M89: raise SIGALRM in `seconds`, and report what was left of the
 * previous alarm. 0 cancels.
 *
 * The resolution is a scheduler tick, which is 10 ms - so alarm(1) fires
 * between 1.00 and 1.01 seconds from now. That is stated rather than
 * rounded away because a program timing something short is entitled to
 * know. There is one alarm per process and no setitimer beside it; see
 * SYS_alarm. */
unsigned int alarm(unsigned int seconds);

/* M89: POSIX puts getopt here and the long-option form in <getopt.h>.
 * The declarations are the same ones; see that header for the two
 * behaviours that differ between implementations. */
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

int symlink(const char *target, const char *path);
/* M93: a hard link. Note the argument order is the opposite way round
 * from symlink's, and that is POSIX's doing rather than this project's -
 * link(existing, new) reads as a copy, symlink(target, path) reads as an
 * assignment, and every Unix has had both spellings since V7. */
int link(const char *old_path, const char *new_path);
/* M93. On this machine a file's contents are already durable when write()
 * returns (leanfs and M92's cache are both write-through); what this
 * pushes is the metadata that write dirtied. See SYS_fsync. fdatasync is
 * the same call - there is no metadata this could skip and still be
 * honest about. */
int fsync(int fd);
int fdatasync(int fd);
/* M89: everything, rather than one file. See SYS_sync for why that is a
 * different question from fsync's rather than the same one with a
 * wildcard. Returns nothing, as POSIX specifies, because there is no
 * failure it could report that a caller could act on. */
void sync(void);
long readlink(const char *path, char *buf, size_t bufsiz);

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
