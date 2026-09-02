/* user_space/libc/include/stdlib.h - M63. See string.h's header comment
 * for why this exists and what "ours, not somebody else's" means.
 *
 * malloc and free still come from user_space/lib/malloc.c, which has
 * been most of one piece of a libc since M19. */
#pragma once

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
/* M80 groundwork. */
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
               int (*cmp)(const void *, const void *));
double strtod(const char *s, char **end);

/* M75: the environment. `getenv` returns a pointer into the environment
 * itself, not a copy - the standard contract, and the reason a caller
 * that wants to keep a value past the next setenv has to copy it. See
 * user_space/libc/src/env.c, and <unistd.h> for `environ`. */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *entry);
int clearenv(void);

int abs(int v);
long labs(long v);

/* M88: the encoding is UTF-8, so the longest character is four bytes.
 * A constant rather than a call into the locale, because there is one
 * locale and it does not change - and a program sizing a buffer with
 * this deserves a compile-time answer. The conversions themselves are in
 * <wchar.h>; these three are declared here too because that is where the
 * C standard puts them and where a ported program will look. */
#define MB_CUR_MAX 4

int mblen(const char *s, size_t n);
int mbtowc(wchar_t *dst, const char *src, size_t n);
int wctomb(char *dst, wchar_t c);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);

#define RAND_MAX 32767
int rand(void);
void srand(unsigned int seed);
