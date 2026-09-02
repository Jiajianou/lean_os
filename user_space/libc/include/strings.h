/* user_space/libc/include/strings.h - M89
 *
 * The BSD string functions, which are a separate header from
 * <string.h> for historical reasons every ported program still relies
 * on. `bzero` and `bcopy` are the deprecated ones and are here because
 * programs written before 1990 still call them; the case-insensitive
 * comparisons are the ones anything modern actually wants.
 */
#pragma once

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

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

/* ASCII case folding only, and deliberately: the encoding is UTF-8
 * (M88) but case is a property of a *locale*, and this machine has the
 * C locale. Folding above U+007F would be inventing rules for languages
 * this system knows nothing about - see <locale.h> on why setlocale
 * refuses everything but C. */
char *strcasestr(const char *haystack, const char *needle);

void  bzero(void *dst, size_t n);
void  bcopy(const void *src, void *dst, size_t n);
int   bcmp(const void *a, const void *b, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);

/* First set bit, counting from 1, or 0 for an argument of zero. */
int ffs(int v);

#ifdef __cplusplus
}
#endif
