/* user_space/libc/include/limits.h - M89
 *
 * The numeric limits, and the two kinds of number in here are worth
 * telling apart.
 *
 * The integer ranges are facts about the compiler and this target, and
 * they are the same numbers any x86-64 C implementation reports.
 *
 * The system limits below them are facts about *this machine*, taken
 * from the places that actually enforce them rather than from a
 * standards document: OPEN_MAX is sched.h's MAX_FDS, NAME_MAX and
 * PATH_MAX are leanfs's own, ARG_MAX is what the spawn path will carry.
 * A program that sizes a buffer with one of these gets a number this
 * kernel will honour, which is the entire point of the header - one
 * copied from somewhere else would be a buffer that is wrong here.
 */
#pragma once

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

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255
#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX
#define MB_LEN_MAX 4          /* UTF-8's longest sequence - see <wchar.h> (M88) */
#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535
#define INT_MIN    (-INT_MAX - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295U
#define LONG_MIN   (-LONG_MAX - 1L)
#define LONG_MAX   9223372036854775807L
#define ULONG_MAX  18446744073709551615UL
#define LLONG_MIN  (-LLONG_MAX - 1LL)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL
#define SSIZE_MAX  LONG_MAX

/* ---- this machine's own limits ---------------------------------------
 *
 * Each one names where it is enforced, so a change there is findable
 * from here. */
#define OPEN_MAX   128   /* MAX_FDS, kernel/sched/sched.h */
#define NAME_MAX   255   /* LEANFS_MAX_NAME, kernel/fs/leanfs.h */
#define PATH_MAX   4096  /* LEANFS_MAX_PATH */
#define ARG_MAX    16384 /* what the spawn path carries - kernel/proc/proc.c */
#define LINK_MAX   65535 /* leanfs's nlink field is 32-bit; nothing approaches this */
#define PIPE_BUF   1024  /* SYS_PIPE_CAPACITY, system_api/include/syscall.h */
#define CHILD_MAX  128   /* MAX_TASKS - one process cannot have more children than there are tasks */
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 32
#define TTY_NAME_MAX  32
#define IOV_MAX     16

/* ---- M89: the POSIX minimum-value macros ----------------------------
 *
 * These are NOT this machine's limits - sysconf() and pathconf() report
 * those, and several of them are larger. They are the values POSIX
 * guarantees every conforming system meets, so that a program can size a
 * buffer at compile time without asking. Every implementation defines
 * them identically, because the standard fixes the numbers; they are
 * here so `getconf` can print them and so a program that uses one
 * compiles.
 *
 * The distinction is worth one sentence because it is the thing that
 * confuses people: `_POSIX_OPEN_MAX` is 20 everywhere, including here,
 * while `sysconf(_SC_OPEN_MAX)` on this machine is 128. The first is a
 * promise about every Unix; the second is a fact about this one.
 */
#define _POSIX_AIO_LISTIO_MAX     2
#define _POSIX_AIO_MAX            1
#define _POSIX_ARG_MAX            4096
#define _POSIX_CHILD_MAX          25
#define _POSIX_DELAYTIMER_MAX     32
#define _POSIX_HOST_NAME_MAX      255
#define _POSIX_LINK_MAX           8
#define _POSIX_LOGIN_NAME_MAX     9
#define _POSIX_MAX_CANON          255
#define _POSIX_MAX_INPUT          255
#define _POSIX_NAME_MAX           14
#define _POSIX_NGROUPS_MAX        8
#define _POSIX_OPEN_MAX           20
#define _POSIX_PATH_MAX           256
#define _POSIX_PIPE_BUF           512
#define _POSIX_RE_DUP_MAX         255
#define _POSIX_RTSIG_MAX          8
#define _POSIX_SEM_NSEMS_MAX      256
#define _POSIX_SEM_VALUE_MAX      32767
#define _POSIX_SIGQUEUE_MAX       32
#define _POSIX_SSIZE_MAX          32767
#define _POSIX_STREAM_MAX         8
#define _POSIX_SYMLINK_MAX        255
#define _POSIX_SYMLOOP_MAX        8
#define _POSIX_THREAD_DESTRUCTOR_ITERATIONS 4
#define _POSIX_THREAD_KEYS_MAX    128
#define _POSIX_THREAD_THREADS_MAX 64
#define _POSIX_TIMER_MAX          32
#define _POSIX_TTY_NAME_MAX       9
#define _POSIX_TZNAME_MAX         6
#define _POSIX_CLOCKRES_MIN       20000000

#define _POSIX2_BC_BASE_MAX       99
#define _POSIX2_BC_DIM_MAX        2048
#define _POSIX2_BC_SCALE_MAX      99
#define _POSIX2_BC_STRING_MAX     1000
#define _POSIX2_CHARCLASS_NAME_MAX 14
#define _POSIX2_COLL_WEIGHTS_MAX  2
#define _POSIX2_EXPR_NEST_MAX     32
#define _POSIX2_LINE_MAX          2048
#define _POSIX2_RE_DUP_MAX        255

#define _XOPEN_IOV_MAX            16
#define _XOPEN_NAME_MAX           255
#define _XOPEN_PATH_MAX           1024

#ifndef SSIZE_MAX
#define SSIZE_MAX  0x7fffffffffffffffL
#endif
#ifndef LONG_BIT
#define LONG_BIT   64
#endif
#ifndef WORD_BIT
#define WORD_BIT   32
#endif

#ifdef __cplusplus
}
#endif
