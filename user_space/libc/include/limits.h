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
