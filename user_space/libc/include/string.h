/* user_space/libc/include/string.h
 *
 * M63: a libc subset - ours, not somebody else's.
 *
 * The ground rules say no external library is linked into anything this
 * OS ships, and that stays true: writing these is the opposite of
 * linking newlib. What they are for is third-party source, which
 * includes <string.h> and expects these names - `user_space/lib/str.h`
 * has had most of them all along under a deliberately different header
 * name, and this is that header under the name the rest of the world
 * uses.
 *
 * The implementations are *shared*, not duplicated: memcpy, memset,
 * strlen and strcmp still come from str.c, and libc/src/string.c adds
 * only what was missing. Two copies of memcpy in one binary is exactly
 * the kind of thing that quietly diverges.
 */
#pragma once

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);

/* M80 groundwork. Returns the E* name rather than a sentence, because a
 * sentence would be describing a failure this system did not report -
 * see <errno.h> on why errno here is coarse. "EINVAL" is true and
 * useful; "Invalid argument" would be a translation of a guess. */
char *strerror(int errnum);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
/* M80 groundwork. */
char *strpbrk(const char *s, const char *accept);
char *strdup(const char *s);
/* M89: at most `n` bytes, always NUL-terminated. Unlike strdup this
 * cannot be expressed with strlen plus malloc without reading past the
 * end of a string that is not terminated within n, which is exactly the
 * case it exists for. */
char *strndup(const char *s, size_t n);
/* M89: strcpy that returns a pointer to the NUL it wrote rather than to
 * the start. The whole point is concatenating without rescanning, which
 * is what makes a loop of them linear instead of quadratic. */
char *stpcpy(char *dst, const char *src);
char *stpncpy(char *dst, const char *src, size_t n);
/* M89: strstr for bytes rather than for strings - the needle and the
 * haystack may both contain NULs, which is why it cannot be written with
 * strstr and why a program scanning a downloaded buffer uses it. */
void *memmem(const void *haystack, size_t hlen, const void *needle, size_t nlen);
/* M89: copy until `c` is copied or `n` bytes are, and return the byte
 * after the copied `c` - or NULL if it was not found. The return is the
 * whole point: it is how a caller tells "I found the terminator" from "I
 * ran out of room", which memcpy plus memchr cannot do in one pass. */
void *memccpy(void *dst, const void *src, int c, size_t n);
/* M89: strlen bounded by `n`, for a field that may not be terminated -
 * a tar header's name is the case it exists for, and reading past it
 * with strlen is the bug it prevents. */
size_t strnlen(const char *s, size_t n);
