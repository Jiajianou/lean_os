#pragma once

#include <stddef.h>
#include <sys/types.h>

#define _POSIX_VERSION 200809L

#define _POSIX_THREADS 200809L

/* The Timers option: clock_getres, clock_gettime, clock_settime, nanosleep
   and the timer_* family, all of which are here. _POSIX_MONOTONIC_CLOCK
   above is defined in terms of this one, and claimed it from M96 until M145
   built the half that was missing. */
#define _POSIX_TIMERS 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L

#ifdef __cplusplus
extern "C" {
#endif

extern char **environ;

int __lean_path_errno(const char *path, int creating);
int __lean_fd_errno(int fd);

int chdir(const char *path);
char *getcwd(char *buf, size_t size);

int getpid(void);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char *path, int mode);

int rmdir(const char *path);
int unlink(const char *path);

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

long read(int fd, void *buf, size_t count);
long write(int fd, const void *buf, size_t count);

ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int close(int fd);

void _exit(int status) __attribute__((noreturn));
int getentropy(void *buf, size_t len);
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t len);
long lseek(int fd, long offset, int whence);
int dup2(int oldfd, int newfd);
/* The kernel's own thread identifier. A thread here IS a task, so this is
   the same number getpid() returns in a single-threaded process and the same
   one pthread_self() gives - which is why the libc has not needed the name
   until something portable asked for it. */
pid_t gettid(void);
int pipe2(int fd[2], int flags);
int ftruncate(int fd, off_t length);
int ftruncate64(int fd, off_t length);
int truncate(const char *path, off_t length);

uid_t getuid(void);
uid_t geteuid(void);
char *getlogin(void);
gid_t getgid(void);
gid_t getegid(void);

int chown(const char *path, uid_t uid, gid_t gid);
int fchown(int fd, uid_t uid, gid_t gid);
int lchown(const char *path, uid_t uid, gid_t gid);
int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags);

int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t uid);
int setegid(gid_t gid);
int setgroups(size_t size, const gid_t *list);

pid_t getppid(void);

pid_t setsid(void);
pid_t getsid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
int setpgrp(void);

int dup(int oldfd);

int chroot(const char *path);

int fchdir(int fd);

int gethostname(char *name, size_t len);
int sethostname(const char *name, size_t len);

pid_t vfork(void);

#define _SC_PAGESIZE      1
#define _SC_PAGE_SIZE     _SC_PAGESIZE
#define _SC_OPEN_MAX      2
#define _SC_NPROCESSORS_ONLN 3
#define _SC_CLK_TCK       4
#define _SC_PHYS_PAGES    5
#define _SC_AVPHYS_PAGES  6

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

#define _SC_XOPEN_CRYPT            300
#define _SC_XOPEN_ENH_I18N         301
#define _SC_XOPEN_REALTIME         302
#define _SC_XOPEN_REALTIME_THREADS 303
#define _SC_XOPEN_SHM              304
#define _SC_XOPEN_STREAMS          305
#define _SC_XOPEN_UNIX             306
#define _SC_XOPEN_UUCP             307
#define _SC_XOPEN_VERSION          308

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
#define _SC_GETPW_R_SIZE_MAX  436
#define _SC_GETGR_R_SIZE_MAX  437

long sysconf(int name);

#define _CS_PATH    1
#define _CS_V7_ENV  2
#define _CS_V6_ENV  3
size_t confstr(int name, char *buf, size_t len);

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

int getpagesize(void);

unsigned int sleep(unsigned int seconds);
int pause(void);
int usleep(unsigned int usec);

unsigned int alarm(unsigned int seconds);

extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

int symlink(const char *target, const char *path);
int link(const char *old_path, const char *new_path);
int fsync(int fd);
int fdatasync(int fd);
void sync(void);
long readlink(const char *path, char *buf, size_t bufsiz);

int unlink(const char *path);
int pipe(int fds[2]);

pid_t fork(void);

int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int execle(const char *path, const char *arg, ...);

long spawnv(const char *path, char *const argv[]);
long spawnve(const char *path, char *const argv[], char *const envp[]);

#ifdef __cplusplus
}
#endif
