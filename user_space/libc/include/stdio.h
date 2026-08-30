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

#include <stdarg.h>
#include <stddef.h>

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
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
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
