/* user_space/libc/include/stdio.h - M63, over M59's descriptors.
 *
 * A FILE here is a descriptor and a little buffering, and nothing more.
 * There is no stream layer to speak of because M59 gave this OS real
 * descriptors and there is nothing a stream layer would add that a
 * program porting to a new OS is entitled to expect - what it is
 * entitled to expect is that printf formats correctly, which is the part
 * this file actually spends its lines on.
 *
 * Deliberately absent: scanf and its family. Nothing has asked, parsing
 * is where a printf-shaped library gets big, and a program that wants it
 * says so at link time - which is the specification, exactly as M63's
 * own "pick the program first" bullet says. */
#pragma once
/* M98: the traditional guard macro as well, and it is interface rather
 * than redundancy: ported code asks "has <stdio.h> been included" by
 * testing for a known guard - gmp.h checks thirteen spellings from
 * thirteen libcs and only declares its FILE* functions if one is
 * defined. `#pragma once` defines nothing, so to that test this stdio.h
 * did not exist and gmp-impl.h's own prototypes went implicit. The
 * glibc spelling, because it is the one everything tests first. */
#define _STDIO_H 1

#include <stdarg.h>
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

typedef struct FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
/* M80 groundwork. Wraps an already-open descriptor in a FILE; `mode` is
 * accepted and ignored, because the descriptor's own access was decided
 * when it was opened and this cannot change it. The FILE does not own
 * the descriptor: fclose on it closes the fd, which is what every
 * implementation does and what a caller has to know. */
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *f);
/* M100: `/bin/sh -c command` with its stdout ("r") or stdin ("w") on a
 * pipe this stream is the other end of. pclose closes the stream, waits
 * for the command, and returns its wait status - or -1 for a stream
 * popen did not open. See popen.c for who asked. */
FILE *popen(const char *command, const char *mode);
int pclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);

/* ---- M97: the five <cstdio> names a C++ library insists on -----------
 *
 * <cstdio> does `using ::fgetpos;` and the rest unconditionally, so an
 * absent declaration stops the standard library compiling rather than
 * costing a program a feature.
 *
 * fpos_t is an OPAQUE type by specification - the standard says a
 * program may only obtain one from fgetpos and hand it back to fsetpos,
 * never do arithmetic on it - which is why it is a struct here rather
 * than a typedef for long. A program that tries to add to one gets a
 * compile error, which is the standard's intent and is friendlier than
 * silently working on this OS and failing on a system where a position
 * carries a multibyte conversion state as well as an offset. */
typedef struct {
    long __pos;
} fpos_t;

int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);
FILE *freopen(const char *path, const char *mode, FILE *f);
FILE *tmpfile(void);
int fflush(FILE *f);
int feof(FILE *f);
/* M80 groundwork - see the implementations for what each one can and
 * cannot honestly do on this system. */
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);
void rewind(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
int ungetc(int c, FILE *f);
void perror(const char *s);
int remove(const char *path);
int rename(const char *from, const char *to);

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 1024
#define FOPEN_MAX 16
#define FILENAME_MAX 128

int fgetc(FILE *f);
/* M80 groundwork. Functions rather than macros, because this stdio has
 * no buffer for the fast path a macro exists to take. */
int getc(FILE *f);
int putc(int c, FILE *f);
int getchar(void);
char *fgets(char *buf, int n, FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int putchar(int c);
int puts(const char *s);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *out, size_t n, const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);

/* M94: how many bytes this stream has buffered and not yet written.
 *
 * Always 0, and that is the true answer rather than a stub: this stdio
 * has no buffer at all - every fputc is a write(2), which stdio.c says
 * at length. gnulib's fpending module has a per-platform `#error
 * "Please port fpending.c to your platform!"` and reaches for this
 * symbol first, which is how it arrived; a program uses it to decide
 * whether an fclose can still fail, and on this machine it cannot fail
 * for that reason because there is nothing left to flush.
 *
 * The double underscore is the name every libc uses for it, and it is
 * not this project's choice. */
size_t __fpending(FILE *f);
int vsprintf(char *out, const char *fmt, va_list ap);

/* ---- M89: printf to a descriptor, without a FILE ---------------------
 *
 * `dprintf` is what a program uses when it has a file descriptor and
 * does not want a stream over it - reading a password from /dev/tty is
 * the canonical case and the one toybox uses it for. It is not
 * "debug printf" despite the name every reader expects. */
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, const char *fmt, va_list ap);

/* ---- M89: a line, however long it turns out to be --------------------
 *
 * The difference from fgets, and the reason a ported program uses these
 * instead: fgets needs the caller to guess a size and silently splits a
 * line that does not fit, so a program reading somebody else's file has
 * no correct buffer size to pick. getdelim grows the buffer.
 *
 * `*lineptr` may be NULL with `*n` 0 on the first call, which is the
 * usual way to start; after that the buffer is reused and only grows.
 * The caller frees it. Returns the byte count INCLUDING the delimiter,
 * or -1 at end of file - and a returned length is the right way to
 * handle a line with an embedded NUL, which strlen on the buffer is not.
 */
long getdelim(char **lineptr, size_t *n, int delim, FILE *f);
long getline(char **lineptr, size_t *n, FILE *f);

/* ---- M89: scanf, on strings and streams ------------------------------
 *
 * The conversions this supports are the ones a program actually writes:
 * %d %i %u %o %x %c %s %n %% and %f/%g/%e, with a field width, the `*`
 * suppression flag, and the h/hh/l/ll/z length modifiers. A `%[...]`
 * scanset is supported too, because `sscanf(s, "%[^,]", ...)` is how
 * half the string splitting in the world is written.
 *
 * What is NOT here: %p, %a, and the `m` allocation modifier. Each would
 * be a few more lines and none of them has been asked for by a program,
 * which is M63's rule - and an unsupported conversion stops the scan and
 * returns what it matched so far, rather than being skipped silently.
 */
int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);
int fscanf(FILE *f, const char *fmt, ...);
int scanf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
