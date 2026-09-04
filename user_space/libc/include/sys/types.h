/* user_space/libc/include/sys/types.h - M77
 *
 * The handful of typedefs <dirent.h> and <sys/stat.h> below it need, and
 * nothing else. It grows the way M63's rule says the rest of this libc
 * does: when a program somebody else wrote fails to compile without a
 * name, not when a standard lists one.
 */
#pragma once

/* M94: <stdint.h>, which is the compiler's own and links nothing.
 *
 * Not decoration: every real <sys/types.h> makes `intptr_t` visible, and
 * code written against a Unix relies on it - GCC's own libgcov.h casts
 * through `intptr_t` having included only <sys/types.h>, and libgcc
 * therefore would not compile for this target until this line existed.
 * That is the kind of expectation a header list cannot predict and a
 * real build finds in one line, which is M63's rule arriving at the
 * compiler itself. */
#include <stdint.h>

#include <stddef.h>

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

typedef long          ssize_t;
typedef unsigned long ino_t;
typedef unsigned int  mode_t;
typedef long          off_t;
typedef int           pid_t;
typedef unsigned int  nlink_t;
typedef unsigned long dev_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned long blksize_t;
typedef unsigned long blkcnt_t;

/* ---- M99: the time types, which POSIX puts HERE ----------------------
 *
 * <time.h> declares the functions; <sys/types.h> is where the standard
 * says the types themselves are visible, and a program is entitled to
 * see `time_t` having included this header alone. Autoconf's
 * AC_CHECK_SIZEOF does exactly that - its default includes are
 * sys/types.h, stdio.h, stdlib.h, string.h and no time.h - so CPython's
 * configure decided `sizeof(time_t)` was **0**, wrote SIZEOF_TIME_T 0
 * into pyconfig.h, and the build stopped four hundred files later in
 * Python/pytime.c with `#error "unsupported time_t size"`. A zero from a
 * probe that failed to compile is the worst kind of configure answer:
 * it is not an error, it is a number.
 *
 * The guard macro is how the two headers agree without one including the
 * other: whichever is read first defines the type, and the second sees
 * that it is already there. A plain repeat of the typedef is legal C11
 * and is not legal C99, and this libc is compiled by other people's
 * build systems with other people's -std flags.
 */
#ifndef __lean_time_t_defined
#define __lean_time_t_defined
typedef long time_t;
#endif
#ifndef __lean_clock_t_defined
#define __lean_clock_t_defined
typedef long clock_t;
#endif
typedef long suseconds_t;

#ifdef __cplusplus
}
#endif
