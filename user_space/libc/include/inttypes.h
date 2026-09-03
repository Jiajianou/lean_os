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
 * is the right failure - *until a real program asks*, which is the
 * amendment M98 makes below: readelf prints ELF half-words with PRId16,
 * so the 8- and 16-bit rows exist now, spelled with the h modifiers both
 * of this libc's format engines already parse.
 *
 * M98: and the SCN* family, which was **entirely absent** - this header
 * had the print half of a header whose name is about both. binutils'
 * `bfd/archive.c` reads an archive member's size with
 * `sscanf(hdr.ar_size, "%" SCNu64, ...)`, which on this machine was a
 * format string ending in a bare `%` and a compile error four tokens
 * from anything the program wrote. Found by building binutils for this
 * target; see M98's notes for the other three of the same shape.
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
/* M98: the 8- and 16-bit rows, added the day readelf asked. For printf
 * the h/hh is near-decorative (default promotions have already widened
 * the argument), but it is what C99 says these expand to, and scanf's
 * table below shares the spelling where it is anything but decorative. */
#define PRId8  "hhd"
#define PRIi8  "hhi"
#define PRIu8  "hhu"
#define PRIx8  "hhx"
#define PRIX8  "hhX"
#define PRIo8  "hho"

#define PRId16 "hd"
#define PRIi16 "hi"
#define PRIu16 "hu"
#define PRIx16 "hx"
#define PRIX16 "hX"
#define PRIo16 "ho"

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

/* ---- M98: the scanf half ----------------------------------------------
 *
 * Same table, and it is a different table rather than an alias for the
 * printf one: `scanf` needs the length modifier to size the *pointer* it
 * writes through, so getting one of these wrong corrupts memory rather
 * than mis-printing. They happen to coincide on this target, and the
 * comment above says why there is exactly one target - so they are
 * written out separately anyway, because the day there are two the
 * coincidence is where the bug goes. */
#define SCNd8  "hhd"
#define SCNi8  "hhi"
#define SCNu8  "hhu"
#define SCNx8  "hhx"
#define SCNo8  "hho"

#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNu16 "hu"
#define SCNx16 "hx"
#define SCNo16 "ho"

#define SCNd32 "d"
#define SCNi32 "i"
#define SCNu32 "u"
#define SCNx32 "x"
#define SCNo32 "o"

#define SCNd64 "ld"
#define SCNi64 "li"
#define SCNu64 "lu"
#define SCNx64 "lx"
#define SCNo64 "lo"

#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNuPTR "lu"
#define SCNxPTR "lx"

#define SCNdMAX "ld"
#define SCNuMAX "lu"
#define SCNxMAX "lx"

typedef long          intmax_t;
typedef unsigned long uintmax_t;

intmax_t  strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);

#ifdef __cplusplus
}
#endif
