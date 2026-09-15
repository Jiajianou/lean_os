#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size) __THROW;
int posix_memalign(void **out, size_t alignment, size_t size) __THROW;
void *aligned_alloc(size_t alignment, size_t size) __THROW;
void free(void *ptr) __THROW;
void *calloc(size_t count, size_t size) __THROW;
void *realloc(void *ptr, size_t size) __THROW;

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

int system(const char *command);

int atexit(void (*fn)(void));
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void __cxa_finalize(void *dso);
extern void *__dso_handle;

const char *getprogname(void);
void setprogname(const char *name);
extern char *program_invocation_name;
extern char *program_invocation_short_name;

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
               int (*cmp)(const void *, const void *));
double strtod(const char *s, char **end);
long double strtold(const char *s, char **end);
float strtof(const char *s, char **end);
long long atoll(const char *s);
long long llabs(long long v);

long random(void);
void srandom(unsigned int seed);
char *initstate(unsigned int seed, char *state, size_t n);
char *setstate(char *state);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

int mkstemp(char *tmpl);

char *realpath(const char *path, char *resolved);
char *mkdtemp(char *tmpl);
char *mktemp(char *tmpl);

int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t len);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *entry);
int clearenv(void);

int abs(int v);
long labs(long v);

#define MB_CUR_MAX 4

int mblen(const char *s, size_t n);
int mbtowc(wchar_t *dst, const char *src, size_t n);
int wctomb(char *dst, wchar_t c);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);

#define RAND_MAX 32767
int rand(void);
void srand(unsigned int seed);

#ifdef __cplusplus
}
#endif
