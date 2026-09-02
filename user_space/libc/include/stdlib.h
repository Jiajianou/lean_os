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

/* M94: a function to run at exit(), in reverse order of registration.
 * At most 32, which is POSIX's own minimum; past that this returns -1
 * rather than dropping the registration, because a program told its
 * flush was registered will not flush. Note _exit() deliberately runs
 * none of these - see <unistd.h>. */
int atexit(void (*fn)(void));

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
/* M89: `long double` is the same 64-bit double on this toolchain's ABI
 * for every purpose this library has - nothing here uses x87's 80-bit
 * format - so this is strtod with a wider return type and not a second
 * parser. Said here rather than left for someone to find out by losing
 * precision they thought they had. */
long double strtold(const char *s, char **end);
float strtof(const char *s, char **end);
long long atoll(const char *s);
long long llabs(long long v);

/* ---- M89: temporary files -------------------------------------------
 *
 * `template` ends in exactly six X's, which are replaced in place. Both
 * return with the template rewritten to the name that was actually
 * created, which is what makes them safe to use and the reason neither
 * has a "generate a name and hope" variant here: `tmpnam` is a race by
 * construction and is deliberately absent.
 *
 * The exclusion is real. mkstemp opens with O_CREAT|O_EXCL, and M87 made
 * that atomic inside leanfs's own lock - so of two processes that pick
 * the same name, exactly one creates it and the other tries again.
 * mkdtemp has no equivalent: SYS_mkdir fails if the directory exists,
 * which is the same guarantee arrived at from the other direction.
 */
/* M89: BSD's random(), which is the same generator this libc's rand()
 * already is - see stdlib.c. Provided because a program that wants a
 * better generator than rand() asks for this one by name, and getting a
 * link error for it is worse than getting the generator that is here
 * under a second name. `initstate`/`setstate` are the state-array
 * interface and are honest about having a fixed-size state. */
long random(void);
void srandom(unsigned int seed);
char *initstate(unsigned int seed, char *state, size_t n);
char *setstate(char *state);

int mkstemp(char *template);
char *mkdtemp(char *template);

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
