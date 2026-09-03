/* user_space/libc/include/stdlib.h - M63. See string.h's header comment
 * for why this exists and what "ours, not somebody else's" means.
 *
 * malloc and free still come from user_space/lib/malloc.c, which has
 * been most of one piece of a libc since M19. */
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

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);

/* M94: a function to run at exit(), in reverse order of registration.
 * At most 32, which is POSIX's own minimum; past that this returns -1
 * rather than dropping the registration, because a program told its
 * flush was registered will not flush. Note _exit() deliberately runs
 * none of these - see <unistd.h>. */
/* ---- M97: div/ldiv, and system ---------------------------------------
 *
 * <cstdlib> does `using ::div;` and `using ::ldiv_t;` unconditionally.
 *
 * div exists at all because C89 did not say which way `/` rounds a
 * negative quotient and div did - it truncates toward zero, always. It
 * has been redundant since C99 made `/` do the same, and it is here
 * because a standard header names it, not because anything should call
 * it. The pairing with the remainder in one return is the other half:
 * the two are one instruction on x86-64 and returning them together lets
 * the compiler keep it that way. */
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

/* M97: `system` is declared and always fails.
 *
 * This is M65's rule applied to a libc function rather than to a
 * syscall: "don't build a thing that pretends to enforce something", and
 * its mirror image - do not build a thing that pretends to DO
 * something. There is a shell here and a spawn, so a real system() is
 * writable; what there is not is a reason, because nothing ported here
 * has needed one and the version that would get written without a caller
 * to check it against would be wrong in some way nobody would find.
 *
 * It returns 0 for the `system(NULL)` probe - which asks "is there a
 * command processor", and the honest answer for a function that will
 * refuse every command is no - and -1 for everything else. A program
 * that checks gets told; a program that does not check gets a failure
 * rather than silence. */
int system(const char *command);

int atexit(void (*fn)(void));
/* M97: the C++ ABI's registration, which carries the shared object a
 * destructor belongs to so that dlclose can run its statics without
 * running anybody else's. See env.c. */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void __cxa_finalize(void *dso);
extern void *__dso_handle;

/* ---- M94: what this program is called ---------------------------------
 *
 * `getprogname` is BSD's and `program_invocation_name` is glibc's, and
 * portable code checks for one and then the other - gnulib's
 * `getprogname` module has a per-OS `#error "not ported to this OS"` and
 * that error is how this arrived, in GNU hello's own build. Both
 * spellings are here because both are cheap and a program that finds
 * neither writes its own.
 *
 * Real, not invented: crt0 already receives argv, and `__lean_start`
 * publishes it. The short form is the part after the last '/', which is
 * what a program prints in its own error messages.
 *
 * `setprogname` exists so a program that wants to be called something
 * else in its diagnostics can say so, which is the only thing anybody
 * uses it for. */
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

/* M97: `tmpl`, not `template`. POSIX spells this parameter `template`
 * and so did this header, which is fine until a C++ program includes it
 * - and then it is a syntax error inside a system header, four lines
 * from anything the program wrote. A parameter name in a prototype means
 * nothing to the compiler and everything to the reader, and this reader
 * includes g++. Found by the first C++ program built against this libc. */
/* M98: the two constants every `<stdlib.h>` has had since C89, and this
 * one did not. `bfd/elf-properties.c` calls `_exit(EXIT_FAILURE)` having
 * included only what its own sysdep.h pulls in, which is what a program
 * is entitled to assume. Absent, it is an undeclared identifier in a
 * file that has nothing to do with exit codes. */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

int mkstemp(char *tmpl);

/* M98: the canonical absolute pathname - symlinks followed, "." and
 * ".." resolved by walking. NULL with errno when any component does
 * not exist, which is POSIX's strict form and the one libiberty's
 * lrealpath depends on for a not-yet-created output file. See
 * realpath.c for the undefined behaviour its absence fed to the gcc
 * driver. `resolved` may be NULL, in which case the result is
 * malloc'd. */
char *realpath(const char *path, char *resolved);
char *mkdtemp(char *tmpl);
/* M98: and the unsafe one, which GNU libiberty's choose-temp.c calls by
 * name - so its absence was a build failure in the first file of the
 * first library of binutils. It picks a name that does not exist and
 * returns it, leaving the name unclaimed until the caller creates it,
 * which is the race mkstemp exists to close. Provided because absence is
 * a link error rather than a safer program; see the implementation. */
char *mktemp(char *tmpl);

/* M85 (second attempt): a terminal for a child process.
 *
 * The standard puts these four in <stdlib.h>, which is a historical
 * accident rather than a category - they have nothing to do with the
 * rest of this header. They are also declared in <pty.h> beside openpty
 * and forkpty, and a program that includes either one gets all four.
 * See user_space/libc/src/pty.c. */
int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t len);

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

#ifdef __cplusplus
}
#endif
