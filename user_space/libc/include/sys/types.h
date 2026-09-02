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

#ifdef __cplusplus
}
#endif
