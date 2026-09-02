#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h> /* M94: %ls and %lc, which wprintf is built on */

#include "syscall_wrappers.h"

struct FILE {
    int fd;
    int eof;
    int used;
    /* M80 groundwork: a sticky error flag, so that `ferror` has
     * something to report. Set by a read or write whose syscall came
     * back negative; cleared only by `clearerr`, which is what "sticky"
     * means and what a caller checking it once at the end relies on. */
    int err;
};

/* stdin/stdout/stderr are the three descriptors every process here starts
 * with (or, for stdin in a GUI terminal's child, does not - see
 * gui_terminal.c, which closes fd 0 deliberately). Static rather than
 * allocated so they exist before main does. */
static FILE std_files[3] = {{0, 0, 1, 0}, {1, 0, 1, 0}, {2, 0, 1, 0}};
FILE *stdin = &std_files[0];
FILE *stdout = &std_files[1];
/* fd 2 has never existed in this OS - a process gets stdin and stdout and
 * nothing else (sched.h's fd table). Pointing stderr at fd 1 is the
 * honest mapping: a ported program's diagnostics go where its output
 * goes, which on this desktop is the terminal window that launched it. */
static FILE stderr_file = {1, 0, 1, 0};
FILE *stderr = &stderr_file;

#define FOPEN_MAX_FILES 16
static FILE open_files[FOPEN_MAX_FILES];

FILE *fopen(const char *path, const char *mode) {
    uint32_t flags = 0;
    for (const char *m = mode; *m; m++) {
        if (*m == 'r') flags |= OPEN_READ;
        if (*m == 'w') flags |= OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE;
        if (*m == 'a') flags |= OPEN_WRITE | OPEN_CREATE | OPEN_APPEND;
        if (*m == '+') flags |= OPEN_READ | OPEN_WRITE;
    }
    if (!flags) {
        return (FILE *)0;
    }
    long fd = sys_open(path, flags);
    if (fd < 0) {
        return (FILE *)0;
    }
    for (int i = 0; i < FOPEN_MAX_FILES; i++) {
        if (!open_files[i].used) {
            open_files[i].fd = (int)fd;
            open_files[i].eof = 0;
            open_files[i].err = 0;
            open_files[i].used = 1;
            return &open_files[i];
        }
    }
    sys_close((int)fd);
    return (FILE *)0;
}

int fclose(FILE *f) {
    if (!f || !f->used) {
        return EOF;
    }
    sys_close(f->fd);
    if (f >= open_files && f < open_files + FOPEN_MAX_FILES) {
        f->used = 0;
    }
    return 0;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f) {
    if (!f || size == 0) {
        return 0;
    }
    long n = sys_read(f->fd, buf, size * count);
    if (n <= 0) {
        f->eof = 1;
        return 0;
    }
    return (size_t)n / size;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    if (!f || size == 0) {
        return 0;
    }
    long n = sys_write(f->fd, buf, size * count);
    if (n <= 0) {
        return 0;
    }
    return (size_t)n / size;
}

int fseek(FILE *f, long offset, int whence) {
    return f && sys_lseek(f->fd, offset, whence) >= 0 ? 0 : -1;
}

long ftell(FILE *f) {
    return f ? sys_lseek(f->fd, 0, SEEK_CUR) : -1;
}

int fflush(FILE *f) {
    (void)f;
    /* Nothing is buffered on the way out - every write is a syscall - so
     * this is honest about having nothing to do rather than pretending. */
    return 0;
}

int feof(FILE *f) {
    return f ? f->eof : 1;
}

/* ---- M80 groundwork: the rest of what a ported program expects -------
 *
 * Each of these is here because CPython's own source stopped the build
 * without it - M63's rule ("let the program name the surface") producing
 * its list at this layer rather than at the syscall one.
 */
int ferror(FILE *f) {
    return f ? f->err : 1;
}

void clearerr(FILE *f) {
    if (f) {
        f->eof = 0;
        f->err = 0;
    }
}

int fileno(FILE *f) {
    return f ? f->fd : -1;
}

void rewind(FILE *f) {
    if (f) {
        sys_lseek(f->fd, 0, SEEK_SET);
        f->eof = 0;
        f->err = 0;
    }
}

/* Accepted and ignored, because there is nothing to configure: this
 * stdio does not buffer at all - every fwrite is a write syscall. That
 * is a real property rather than a stub, and it is why fflush already
 * had nothing to do. A program that calls setvbuf to get *unbuffered*
 * behaviour already has it; one that asks for full buffering gets
 * unbuffered, which is slower and never wrong. */
int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)f;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

void setbuf(FILE *f, char *buf) {
    (void)f;
    (void)buf;
}

/* A one-character pushback, which is all the standard guarantees. Kept
 * per-FILE rather than as a global so two streams cannot steal each
 * other's - and implemented by seeking back rather than by a buffer,
 * because this stdio has no buffer to put it in and a descriptor here
 * has a real position (M59). A stream with no position - stdin, a pipe -
 * cannot take one back, and says so. */
int ungetc(int c, FILE *f) {
    if (!f || c == EOF) {
        return EOF;
    }
    long pos = sys_lseek(f->fd, 0, SEEK_CUR);
    if (pos <= 0) {
        return EOF; /* not seekable, or already at the start */
    }
    if (sys_lseek(f->fd, pos - 1, SEEK_SET) < 0) {
        return EOF;
    }
    f->eof = 0;
    return c;
}

/* Writes `s`, a colon, and this system's one honest description of what
 * went wrong. There is no errno string table here because there is
 * barely an errno (see <errno.h>): a syscall that failed said -1 and
 * nothing else, so inventing "No such file or directory" for it would be
 * a guess printed as a fact. */
void perror(const char *s) {
    if (s && s[0]) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs("failed\n", stderr);
}

int remove(const char *path) {
    return (int)sys_unlink(path);
}

int rename(const char *from, const char *to) {
    return (int)sys_rename(from, to);
}

int fgetc(FILE *f) {
    char c;
    if (!f || sys_read(f->fd, &c, 1) != 1) {
        if (f) {
            f->eof = 1;
        }
        return EOF;
    }
    return (unsigned char)c;
}

char *fgets(char *buf, int n, FILE *f) {
    if (n <= 0) {
        return (char *)0;
    }
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    if (i == 0) {
        return (char *)0;
    }
    buf[i] = '\0';
    return buf;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    return (f && sys_write(f->fd, &ch, 1) == 1) ? c : EOF;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return (f && sys_write(f->fd, s, n) == (long)n) ? 0 : EOF;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) {
        return EOF;
    }
    return fputc('\n', stdout);
}

/* ---- the formatter ---------------------------------------------------
 *
 * Everything above is plumbing; this is the part a ported program
 * actually depends on being right. One implementation, into a caller's
 * buffer, and printf/fprintf are that plus a write - so there is exactly
 * one place %e can be wrong.
 */

typedef struct {
    char *buf;
    size_t cap;
    size_t len; /* what *would* have been written - snprintf's return value */
} sink_t;

static void emit(sink_t *s, char c) {
    if (s->buf && s->len + 1 < s->cap) {
        s->buf[s->len] = c;
    }
    s->len++;
}

static void emit_str(sink_t *s, const char *str, int len) {
    for (int i = 0; i < len; i++) {
        emit(s, str[i]);
    }
}

static void emit_pad(sink_t *s, char pad, int n) {
    for (int i = 0; i < n; i++) {
        emit(s, pad);
    }
}

/* An unsigned value in any base, into a caller-supplied scratch buffer,
 * returning its length. Digits come out backwards and are reversed by
 * the caller's emit loop - the usual arrangement, and the reason this
 * hands back a buffer rather than emitting directly is that width and
 * precision both need the length before the first digit is written. */
static int format_uint(unsigned long long v, int base, int upper, char *out) {
    static const char LOWER[] = "0123456789abcdef";
    static const char UPPER[] = "0123456789ABCDEF";
    const char *digits = upper ? UPPER : LOWER;
    int n = 0;
    do {
        out[n++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    } while (v);
    for (int i = 0; i < n / 2; i++) {
        char t = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = t;
    }
    return n;
}

/* Rounded decimal digits of |v| < 1, `prec` of them, into `out`. Used by
 * both %f and %e, which is what keeps their rounding identical. */
static void format_frac(double v, int prec, char *out) {
    for (int i = 0; i < prec; i++) {
        v *= 10.0;
        int d = (int)v;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        out[i] = (char)('0' + d);
        v -= (double)d;
    }
    /* Round the last digit from what is left over, carrying upward
     * through the string. A carry out of the top is the caller's
     * problem - both callers below normalise before calling. */
    if (v >= 0.5) {
        for (int i = prec - 1; i >= 0; i--) {
            if (out[i] != '9') {
                out[i]++;
                return;
            }
            out[i] = '0';
        }
    }
}

static int is_nan(double v) { return v != v; }
static int is_inf(double v) { return v != 0.0 && v * 0.5 == v; }

static void emit_double(sink_t *s, double v, int prec, int sci, int upper) {
    if (is_nan(v)) {
        emit_str(s, upper ? "NAN" : "nan", 3);
        return;
    }
    if (v < 0.0 || (v == 0.0 && 1.0 / v < 0.0)) {
        emit(s, '-');
        v = -v;
    }
    if (is_inf(v)) {
        emit_str(s, upper ? "INF" : "inf", 3);
        return;
    }
    if (prec < 0) {
        prec = 6;
    }
    if (prec > 17) {
        prec = 17; /* past what a double distinguishes - more digits would be invention */
    }

    int exp10 = 0;
    if (sci && v != 0.0) {
        while (v >= 10.0) {
            v /= 10.0;
            exp10++;
        }
        while (v < 1.0) {
            v *= 10.0;
            exp10--;
        }
    }

    char frac[20];
    double ipart_d = (double)(unsigned long long)v;
    double fpart = v - ipart_d;
    format_frac(fpart, prec, frac);
    unsigned long long ipart = (unsigned long long)v;
    /* format_frac may have carried out of its top digit, which shows up
     * as every digit being '0' when the leftover was >= 0.5. */
    if (prec > 0 && fpart >= 0.5) {
        int all_zero = 1;
        for (int i = 0; i < prec; i++) {
            if (frac[i] != '0') {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            ipart++;
            if (sci && ipart >= 10) {
                ipart = 1;
                exp10++;
            }
        }
    } else if (prec == 0 && fpart >= 0.5) {
        ipart++;
        if (sci && ipart >= 10) {
            ipart = 1;
            exp10++;
        }
    }

    char ibuf[24];
    int ilen = format_uint(ipart, 10, 0, ibuf);
    emit_str(s, ibuf, ilen);
    if (prec > 0) {
        emit(s, '.');
        emit_str(s, frac, prec);
    }
    if (sci) {
        emit(s, upper ? 'E' : 'e');
        emit(s, exp10 < 0 ? '-' : '+');
        int e = exp10 < 0 ? -exp10 : exp10;
        char ebuf[8];
        int elen = format_uint((unsigned long long)e, 10, 0, ebuf);
        if (elen < 2) {
            emit(s, '0'); /* the standard's two-digit minimum exponent */
        }
        emit_str(s, ebuf, elen);
    }
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
    sink_t s = {out, n, 0};

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit(&s, *p);
            continue;
        }
        p++;
        if (*p == '%') {
            emit(&s, '%');
            continue;
        }

        int left = 0, zero = 0, plus = 0, space = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else break;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p++ - '0');
            }
        }
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    prec = prec * 10 + (*p++ - '0');
                }
            }
        }
        int lng = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z') {
            if (*p == 'l' || *p == 'z') {
                lng++;
            }
            p++;
        }

        /* Everything is formatted into `body` first so width can be
         * applied uniformly - a conversion that emitted directly would
         * need to know its own length twice. */
        char body[512];
        sink_t b = {body, sizeof(body), 0};
        char conv = *p;

        if (conv == 'd' || conv == 'i') {
            long long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long long mag = (unsigned long long)(v < 0 ? -v : v);
            char nbuf[24];
            int nlen = format_uint(mag, 10, 0, nbuf);
            if (v < 0) emit(&b, '-');
            else if (plus) emit(&b, '+');
            else if (space) emit(&b, ' ');
            if (prec > nlen) {
                emit_pad(&b, '0', prec - nlen);
            }
            emit_str(&b, nbuf, nlen);
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            unsigned long long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            int base = conv == 'u' ? 10 : (conv == 'o' ? 8 : 16);
            char nbuf[24];
            int nlen = format_uint(v, base, conv == 'X', nbuf);
            if (prec > nlen) {
                emit_pad(&b, '0', prec - nlen);
            }
            emit_str(&b, nbuf, nlen);
        } else if (conv == 'p') {
            unsigned long long v = (unsigned long long)va_arg(ap, void *);
            char nbuf[24];
            int nlen = format_uint(v, 16, 0, nbuf);
            emit_str(&b, "0x", 2);
            emit_str(&b, nbuf, nlen);
        } else if (conv == 'c') {
            /* M94: `%lc` is a wide character and converts to whatever
             * bytes it takes in this locale's encoding, which is UTF-8
             * (M88). Written here rather than in a second wide formatter
             * because `%lc` is what a NARROW printf does with one, and
             * <wchar.h>'s wprintf is built on this same call. */
            if (lng) {
                char mb[8];
                int mn = (int)wcrtomb(mb, (wchar_t)va_arg(ap, unsigned int),
                                      (mbstate_t *)0);
                if (mn > 0) {
                    emit_str(&b, mb, mn);
                }
            } else {
                emit(&b, (char)va_arg(ap, int));
            }
        } else if (conv == 's' && lng) {
            /* M94: `%ls` - a wide string, converted. GNU hello's
             * `wprintf (L"%ls\n", ...)` is what asked for it, which is
             * M63's rule reaching the formatter: a conversion nobody
             * here would have written, named by a program somebody else
             * wrote.
             *
             * The precision is in BYTES of the converted output, not in
             * wide characters, which is what C specifies and is the one
             * thing about `%.*ls` that is easy to get backwards. */
            const wchar_t *ws = va_arg(ap, const wchar_t *);
            if (!ws) {
                emit_str(&b, "(null)", 6);
            } else {
                for (int i = 0; ws[i]; i++) {
                    char mb[8];
                    int mn = (int)wcrtomb(mb, ws[i], (mbstate_t *)0);
                    if (mn <= 0) {
                        break;
                    }
                    if (prec >= 0 && (int)b.len + mn > prec) {
                        break;
                    }
                    emit_str(&b, mb, mn);
                }
            }
        } else if (conv == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str) {
                str = "(null)";
            }
            int len = (int)strlen(str);
            if (prec >= 0 && prec < len) {
                len = prec;
            }
            emit_str(&b, str, len);
        } else if (conv == 'f' || conv == 'F') {
            emit_double(&b, va_arg(ap, double), prec, 0, conv == 'F');
        } else if (conv == 'e' || conv == 'E') {
            emit_double(&b, va_arg(ap, double), prec, 1, conv == 'E');
        } else if (conv == 'g' || conv == 'G') {
            /* %g's real rule is "whichever of %e and %f is shorter, with
             * trailing zeros removed". This does the exponent test and
             * skips the trailing-zero trim, which is the half that
             * changes what a number *is* rather than how it looks. */
            double v = va_arg(ap, double);
            double mag = v < 0 ? -v : v;
            int sci = (mag != 0.0 && (mag < 1e-4 || mag >= 1e6));
            emit_double(&b, v, prec < 0 ? 6 : prec, sci, conv == 'G');
        } else if (conv == '\0') {
            break;
        } else {
            emit(&b, '%');
            emit(&b, conv);
        }

        int blen = (int)(b.len < sizeof(body) ? b.len : sizeof(body) - 1);
        int pad = width - blen;
        if (pad > 0 && !left) {
            emit_pad(&s, zero && prec < 0 ? '0' : ' ', pad);
        }
        emit_str(&s, body, blen);
        if (pad > 0 && left) {
            emit_pad(&s, ' ', pad);
        }
    }

    if (out && n > 0) {
        out[s.len < n ? s.len : n - 1] = '\0';
    }
    return (int)s.len;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    /* One buffer, one write. A formatter that wrote a syscall per
     * character would make every printf in a ported program hundreds of
     * syscalls, which on this OS is hundreds of ring transitions. */
    static char line[1024];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    int len = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1;
    if (len > 0) {
        sys_write(f->fd, line, (size_t)len);
    }
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

void __assert_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "assertion failed: %s at %s:%d\n", expr, file, line);
    abort();
}

/* M80 groundwork. Real functions rather than the macros they are
 * elsewhere: a macro exists to skip a call on the buffered fast path,
 * and this stdio has no buffer to make one. */
int getc(FILE *f) {
    return fgetc(f);
}

int putc(int c, FILE *f) {
    return fputc(c, f);
}

int getchar(void) {
    return fgetc(stdin);
}

/* M80 groundwork - see <stdio.h>. The FILE takes over the descriptor:
 * fclose on the result closes it, which is what every implementation
 * does and what a caller has to know. */
FILE *fdopen(int fd, const char *mode) {
    (void)mode; /* the descriptor's access was decided when it was opened */
    if (fd < 0) {
        return (FILE *)0;
    }
    for (int i = 0; i < FOPEN_MAX_FILES; i++) {
        if (!open_files[i].used) {
            open_files[i].fd = fd;
            open_files[i].eof = 0;
            open_files[i].err = 0;
            open_files[i].used = 1;
            return &open_files[i];
        }
    }
    return (FILE *)0;
}

/* ---- M89: printf to a bare descriptor -------------------------------
 *
 * The same one-buffer-one-write shape as vfprintf above and for the same
 * reason. It does not go through a FILE because its whole point is that
 * the caller has a descriptor and no stream - see <stdio.h>.
 */
int vdprintf(int fd, const char *fmt, va_list ap) {
    static char line[1024];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    int len = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1;
    if (len > 0) {
        sys_write(fd, line, (size_t)len);
    }
    return n;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
    return vsnprintf(out, (size_t)-1, fmt, ap);
}

/* ---- M89: getdelim/getline ------------------------------------------
 *
 * See <stdio.h> for why a ported program uses these rather than fgets.
 *
 * The growth policy is doubling from 128, which matters more here than
 * it would elsewhere: this stdio is unbuffered, so every byte is a
 * syscall, and a realloc per byte on top of that would make reading a
 * file quadratic in a way a person would notice. The buffer is always
 * NUL-terminated even though the return value is the length, because
 * every caller in the world treats it as a string as well.
 */
long getdelim(char **lineptr, size_t *n, int delim, FILE *f) {
    if (!lineptr || !n || !f) {
        return -1;
    }
    if (!*lineptr || *n == 0) {
        size_t want = 128;
        char *p = (char *)malloc(want);
        if (!p) {
            return -1;
        }
        *lineptr = p;
        *n = want;
    }
    size_t len = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) {
            if (len == 0) {
                return -1; /* nothing read at all - end of file */
            }
            break;
        }
        if (len + 2 > *n) {
            size_t want = *n * 2;
            char *p = (char *)realloc(*lineptr, want);
            if (!p) {
                return -1;
            }
            *lineptr = p;
            *n = want;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim) {
            break;
        }
    }
    (*lineptr)[len] = '\0';
    return (long)len;
}

long getline(char **lineptr, size_t *n, FILE *f) {
    return getdelim(lineptr, n, '\n', f);
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

size_t __fpending(FILE *f) {
    (void)f;
    return 0; /* there is no buffer - see <stdio.h> */
}
