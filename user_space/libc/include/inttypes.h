/* user_space/libc/include/inttypes.h - M80 groundwork
 *
 * `<stdint.h>` plus the printf/scanf length macros. The types come from
 * the compiler - a freestanding GCC provides <stdint.h> itself, which is
 * why this project has never needed one of its own - so what this file
 * actually adds is the PRI* family, which is the half real programs
 * include it for.
 *
 * Written out for the 64-bit types only, plus the pointer-sized ones,
 * because those are the ones whose spelling actually differs between
 * platforms and therefore the ones a program cannot hardcode. A program
 * asking for PRId8 gets a compile error rather than a wrong format, which
 * is the right failure.
 */
#pragma once

#include <stdint.h>

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

/* On this target: int is 32-bit, long and long long are both 64-bit, and
 * a pointer is 64-bit. So the 64-bit and pointer-sized macros are "l" and
 * the 32-bit ones are "". Stated here rather than derived, because there
 * is exactly one target. */
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRIo32 "o"

#define PRId64 "ld"
#define PRIi64 "li"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIo64 "lo"

#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

#define PRIdMAX "ld"
#define PRIuMAX "lu"
#define PRIxMAX "lx"

typedef long          intmax_t;
typedef unsigned long uintmax_t;

intmax_t  strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);

#ifdef __cplusplus
}
#endif
