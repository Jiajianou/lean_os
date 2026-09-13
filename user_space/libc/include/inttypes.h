#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
