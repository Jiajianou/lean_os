#pragma once
#define _STDIO_H 1

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

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
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *f);
FILE *popen(const char *command, const char *mode);
int pclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);

/* off_t is long on this system, so these are fseek and ftell under the
   names a large-file interface uses rather than a wider pair beside
   them. A target whose off_t outgrew long would have to split them. */
int fseeko(FILE *f, off_t offset, int whence);
off_t ftello(FILE *f);

typedef struct {
    long __pos;
} fpos_t;

int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);
FILE *freopen(const char *path, const char *mode, FILE *f);
FILE *tmpfile(void);
int fflush(FILE *f);
int feof(FILE *f);
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

int asprintf(char **out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vasprintf(char **out, const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);

size_t __fpending(FILE *f);
int vsprintf(char *out, const char *fmt, va_list ap);

int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, const char *fmt, va_list ap);

long getdelim(char **lineptr, size_t *n, int delim, FILE *f);
long getline(char **lineptr, size_t *n, FILE *f);

int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);
int fscanf(FILE *f, const char *fmt, ...);
int scanf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
