#include <errno.h>
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
    /* M98: the one character ungetc pushed back, or -1. It has to be a
     * real slot rather than the lseek(-1) trick that stood here, because
     * C lets a caller push back a character DIFFERENT from the one it
     * read - and gas does, in its very first act on every input file:
     * it reads '#' then ' ', pushes back '#', and the seek-based version
     * silently handed it the space instead. The '#' vanished, line one
     * of every assembly file became code, and the machine's own `as`
     * could not assemble a comment. Every reader consults this first. */
    int unget;
    /* ---- M98: a write buffer, and the measurement that asked for it ---
     *
     * There was none. Every `fputc`, `fputs`, `fwrite` and `fprintf` was
     * one `write` syscall, and `fflush` said so honestly: *"nothing is
     * buffered on the way out - every write is a syscall"*.
     *
     * M98 counted them. Profiling one C++ compile on this machine with
     * M101's syscall counter: **221,676 syscalls, of which 199,385 -
     * 89% - were `write`**, producing a 1.5 MB assembly file at about
     * seven bytes per call, and `vfs_handle_write` was 14% of all kernel
     * time in the sampler's histogram. A compiler writing its output
     * one `fprintf` at a time is the ordinary case, not an unusual one.
     *
     * `mode` is one of the three <stdio.h> names: fully buffered (a
     * file), line buffered (a terminal), or unbuffered. The defaults are
     * the ones every Unix picks and each for a reason that matters here:
     * a file is fully buffered because that is where the syscalls were;
     * stdout is line buffered so a prompt appears before the read that
     * follows it; stderr is unbuffered so a diagnostic survives the
     * crash that produced it.
     *
     * The buffer is IN the FILE rather than allocated, because a FILE
     * here is one of nineteen static structures (three standard streams
     * plus FOPEN_MAX_FILES) and an allocation would make `fopen` able to
     * fail in a second way. 1024 bytes is BUFSIZ, which is what a
     * program that asks gets told. */
    char wbuf[BUFSIZ];
    int wlen;
    int mode;
};

/* stdin/stdout/stderr are the three descriptors every process here starts
 * with (or, for stdin in a GUI terminal's child, does not - see
 * gui_terminal.c, which closes fd 0 deliberately). Static rather than
 * allocated so they exist before main does. */
static FILE std_files[3] = {
    {0, 0, 1, 0, -1, {0}, 0, _IOLBF},
    {1, 0, 1, 0, -1, {0}, 0, _IOLBF},
    {2, 0, 1, 0, -1, {0}, 0, _IOLBF},
};
FILE *stdin = &std_files[0];
FILE *stdout = &std_files[1];
/* fd 2 has never existed in this OS - a process gets stdin and stdout and
 * nothing else (sched.h's fd table). Pointing stderr at fd 1 is the
 * honest mapping: a ported program's diagnostics go where its output
 * goes, which on this desktop is the terminal window that launched it. */
static FILE stderr_file = {1, 0, 1, 0, -1, {0}, 0, _IONBF};
FILE *stderr = &stderr_file;

#define FOPEN_MAX_FILES 16
static FILE open_files[FOPEN_MAX_FILES];

/* ---- M98: the buffer's four operations ------------------------------
 *
 * Everything that writes goes through `stream_put`, and everything that
 * has to see the file as the kernel sees it - a read, a seek, a close,
 * exit - calls `stream_flush` first. Missing one of those is the classic
 * buffering bug and is why there is exactly one of each rather than a
 * flush at every call site. */
static int stream_flush(FILE *f) {
    if (!f || f->wlen == 0) {
        return 0;
    }
    int len = f->wlen;
    f->wlen = 0; /* cleared FIRST: a failed write must not be retried
                  * forever by a caller that flushes in a loop, and the
                  * bytes are gone either way. */
    long n = sys_write(f->fd, f->wbuf, (size_t)len);
    if (n != (long)len) {
        f->err = 1;
        return EOF;
    }
    return 0;
}

static int stream_write(FILE *f, const char *p, size_t len) {
    if (!f || len == 0) {
        return 0;
    }
    if (f->mode == _IONBF) {
        /* Unbuffered still goes through here, so that a stream switched
         * to _IONBF mid-life cannot leave buffered bytes behind it. */
        if (stream_flush(f) != 0) {
            return EOF;
        }
        return sys_write(f->fd, p, len) == (long)len ? 0 : EOF;
    }
    for (size_t i = 0; i < len; i++) {
        if (f->wlen == (int)sizeof(f->wbuf)) {
            if (stream_flush(f) != 0) {
                return EOF;
            }
        }
        f->wbuf[f->wlen++] = p[i];
        if (f->mode == _IOLBF && p[i] == '\n') {
            if (stream_flush(f) != 0) {
                return EOF;
            }
        }
    }
    return 0;
}

/* Every stream this libc knows about, for exit(). A fixed list rather
 * than a registry because the set is fixed: three standard streams and
 * FOPEN_MAX_FILES slots, all of them static. */
void __lean_stdio_flush_all(void) {
    for (int i = 0; i < 3; i++) {
        stream_flush(&std_files[i]);
    }
    stream_flush(&stderr_file);
    for (int i = 0; i < FOPEN_MAX_FILES; i++) {
        if (open_files[i].used) {
            stream_flush(&open_files[i]);
        }
    }
}

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
            open_files[i].unget = -1;
            open_files[i].wlen = 0;
            /* A file is fully buffered, which is where M98's 199,385
             * write syscalls were. */
            open_files[i].mode = _IOFBF;
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
    stream_flush(f); /* M98: before the descriptor goes, not after */
    sys_close(f->fd);
    if (f >= open_files && f < open_files + FOPEN_MAX_FILES) {
        f->used = 0;
    }
    return 0;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f) {
    /* M98: a stream opened "r+" can be written and then read, and the
     * kernel's file position is where the buffered bytes have not gone
     * yet. Flush before reading or the read returns what the write was
     * supposed to have replaced. */
    if (f) {
        stream_flush(f);
    }
    if (!f || size == 0 || count == 0) {
        return 0;
    }
    size_t want = size * count;
    size_t got = 0;
    if (f->unget >= 0) {
        ((char *)buf)[0] = (char)f->unget;
        f->unget = -1;
        got = 1;
    }
    if (got < want) {
        long n = sys_read(f->fd, (char *)buf + got, want - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (got == 0) {
            f->eof = 1;
            return 0;
        }
    }
    return got / size;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    if (!f || size == 0 || count == 0) {
        return 0;
    }
    size_t bytes = size * count;
    /* Past the buffer's own size there is nothing to gain by copying:
     * flush what is pending, so ordering is kept, and write the caller's
     * bytes straight through. */
    if (bytes >= sizeof(f->wbuf)) {
        if (stream_flush(f) != 0) {
            return 0;
        }
        long n = sys_write(f->fd, buf, bytes);
        if (n <= 0) {
            f->err = 1;
            return 0;
        }
        return (size_t)n / size;
    }
    if (stream_write(f, (const char *)buf, bytes) != 0) {
        return 0;
    }
    return count;
}

int fseek(FILE *f, long offset, int whence) {
    if (!f) {
        return -1;
    }
    stream_flush(f); /* M98: the pending bytes belong at the OLD position */
    /* A pushed-back character the seek discards was still consumed from
     * the underlying file, so a relative seek has to account for it or
     * land one byte past where the caller's arithmetic says. */
    if (f->unget >= 0) {
        if (whence == SEEK_CUR) {
            offset -= 1;
        }
        f->unget = -1;
    }
    return sys_lseek(f->fd, offset, whence) >= 0 ? 0 : -1;
}

long ftell(FILE *f) {
    if (!f) {
        return -1;
    }
    /* M98: buffered bytes are logically written, so the position a
     * caller is told has to include them. Flushing is the simplest way
     * to make that true and is what ftell costs on every libc that
     * buffers. */
    stream_flush(f);
    long pos = sys_lseek(f->fd, 0, SEEK_CUR);
    if (pos > 0 && f->unget >= 0) {
        pos -= 1; /* the pushed-back character is logically unread */
    }
    return pos;
}

int fflush(FILE *f) {
    /* M98: this used to be honest about having nothing to do, because
     * there was no buffer. There is one now, and fflush(NULL) means
     * "every stream" - which is what a program calls before it forks, or
     * before it does something that might not come back. */
    if (!f) {
        __lean_stdio_flush_all();
        return 0;
    }
    return stream_flush(f);
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
        f->unget = -1;
    }
}

/* Accepted and ignored, because there is nothing to configure: this
 * stdio does not buffer at all - every fwrite is a write syscall. That
 * is a real property rather than a stub, and it is why fflush already
 * had nothing to do. A program that calls setvbuf to get *unbuffered*
 * behaviour already has it; one that asks for full buffering gets
 * unbuffered, which is slower and never wrong. */
/* M98: the mode is real now; the caller's buffer is not.
 *
 * `buf` and `size` are ignored deliberately rather than half-honoured: a
 * FILE here carries its own BUFSIZ buffer inside a static structure, and
 * adopting a caller's array would mean a stream whose buffer can be
 * freed out from under it - which is the one way this can go wrong that
 * a program cannot debug. What a caller actually wants from setvbuf is
 * almost always the MODE (make this unbuffered; line-buffer this), and
 * that is honoured exactly. Returning 0 with the mode applied is the
 * behaviour a program depends on; returning -1 because the buffer was
 * not adopted would make it think buffering is unavailable. */
int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)buf;
    (void)size;
    if (!f || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) {
        return -1;
    }
    stream_flush(f); /* the old mode's pending bytes leave under the old rules */
    f->mode = mode;
    return 0;
}

void setbuf(FILE *f, char *buf) {
    /* The standard's own definition: setvbuf with _IOFBF and BUFSIZ, or
     * _IONBF when the buffer is NULL. */
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
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
    if (f->unget >= 0) {
        return EOF; /* the standard guarantees one character; this is it */
    }
    f->unget = (unsigned char)c;
    f->eof = 0;
    return (unsigned char)c;
}

/* Writes `s`, a colon, and what errno actually says. This printed a
 * bare "failed" for eleven milestones, on the honest ground that errno
 * was barely ever set - but M98 gave stat a real error code and libc
 * has always set the handful it genuinely knows (EEXIST, ERANGE, the
 * wrapper checks), so naming them is reporting rather than guessing.
 * strerror already tells the truth about the rest: a code with no
 * entry is "error", and errno 0 at a failure is its own information -
 * the operation that failed set nothing, which "failed" still says
 * best. */
void perror(const char *s) {
    if (s && s[0]) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    if (errno != 0) {
        fputs(strerror(errno), stderr);
        fputs("\n", stderr);
    } else {
        fputs("failed\n", stderr);
    }
}

int remove(const char *path) {
    return (int)sys_unlink(path);
}

int rename(const char *from, const char *to) {
    return (int)sys_rename(from, to);
}

int fgetc(FILE *f) {
    if (!f) {
        return EOF;
    }
    if (f->unget >= 0) {
        int c = f->unget;
        f->unget = -1;
        return c;
    }
    stream_flush(f); /* M98 - see fread */
    char c;
    if (sys_read(f->fd, &c, 1) != 1) {
        f->eof = 1;
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
    if (!f || stream_write(f, &ch, 1) != 0) {
        return EOF;
    }
    return c;
}

int fputs(const char *s, FILE *f) {
    if (!f) {
        return EOF;
    }
    return stream_write(f, s, strlen(s)) == 0 ? 0 : EOF;
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
 * both %f and %e, which is what keeps their rounding identical. Returns
 * 1 when the rounding carried out of the top digit, so the caller
 * increments the integer part instead of re-deriving the carry from the
 * fraction - the re-derivation is the bug M98's printf test found.
 *
 * A leftover of exactly one half rounds to even (the last digit's
 * parity, or the integer part's when there are no digits), because that
 * is what the hardware's default rounding does and therefore what every
 * other printf on x86 prints: %.0f of 2.5 is 2 everywhere else, and a
 * formatter that says 3 disagrees with the machine it runs on. Exact
 * halves are the only case this distinguishes - a value that is merely
 * near a half has already made its choice in binary. */
/* The exact error of the product p = a*b, by Veltkamp splitting - ten
 * lines of pure double arithmetic that recover what the one rounding in
 * `a*b` threw away. Needed because "is this a tie" cannot be answered
 * from the rounded product alone: -0.00005 scaled by 10^4 lands within
 * half an ulp of 0.5 and rounds TO it, but its true value is above it,
 * so the host prints -0.0001 where a naive comparison says tie-to-even
 * and prints -0.0000. The error term keeps the side. (fma() would be
 * one line, but the target compiler lowers __builtin_fma to a libm call
 * this libc would then have to be, correctly, which is a bigger ask.) */
static double two_prod_err(double a, double b, double p) {
    const double split = 134217729.0; /* 2^27 + 1 */
    double ca = split * a;
    double ah = ca - (ca - a);
    double al = a - ah;
    double cb = split * b;
    double bh = cb - (cb - b);
    double bl = b - bh;
    return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

static int format_frac(double v, int prec, char *out, int ipart_odd) {
    if (prec <= 15) {
        /* One scaling multiply instead of `prec` of them: 10^prec is
         * exact and v*pow10 rounds ONCE, so the comparison against the
         * halfway point - with the multiply's own error recovered
         * above - decides the way the true value does. The iterative
         * version accumulated one rounding per digit, and by the last
         * digit the leftover no longer knew which side of a half it
         * was on. 15 is where 10^prec stops fitting the 53-bit integer
         * range this depends on. */
        double pow10 = 1.0;
        for (int i = 0; i < prec; i++) {
            pow10 *= 10.0;
        }
        double scaled = v * pow10;
        double err = two_prod_err(v, pow10, scaled);
        unsigned long long d = (unsigned long long)scaled;
        double r = scaled - (double)d;
        int odd = prec ? (int)(d & 1) : ipart_odd;
        int up;
        if (r > 0.5) {
            up = 1;
        } else if (r < 0.5) {
            up = 0;
        } else {
            up = err > 0.0 || (err == 0.0 && odd);
        }
        if (up) {
            d++;
        }
        for (int i = prec - 1; i >= 0; i--) {
            out[i] = (char)('0' + (int)(d % 10));
            d /= 10;
        }
        return d != 0; /* what is left after prec digits is the carry out */
    }

    /* Past 15 digits a double's own fraction is exhausted anyway; the
     * digit-at-a-time walk with a plain half-up finish is as honest as
     * the input. */
    for (int i = 0; i < prec; i++) {
        v *= 10.0;
        int d = (int)v;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        out[i] = (char)('0' + d);
        v -= (double)d;
    }
    if (v >= 0.5) {
        for (int i = prec - 1; i >= 0; i--) {
            if (out[i] != '9') {
                out[i]++;
                return 0;
            }
            out[i] = '0';
        }
        return 1;
    }
    return 0;
}

static int is_nan(double v) { return v != v; }
static int is_inf(double v) { return v != 0.0 && v * 0.5 == v; }

/* `alt` is printf's `#` flag: a decimal point is written even when the
 * precision is zero (C99 7.19.6.1). It changes nothing else - the digits
 * are the digits - which is why it is one parameter and not a mode. */
static void emit_double(sink_t *s, double v, int prec, int sci, int upper,
                        int alt) {
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
    unsigned long long ipart = (unsigned long long)v;
    if (format_frac(fpart, prec, frac, (int)(ipart & 1))) {
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
    } else if (alt) {
        emit(s, '.'); /* the whole of what `#` does to a float */
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

/* %g's trailing-zero trim, applied to what emit_double just wrote from
 * `start` onward: zeros after the decimal point go, then a bare point
 * goes, and a mantissa's exponent suffix survives in place. Operates on
 * the sink's buffer directly, which is safe for exactly one caller -
 * the %g branch formats into `body`, whose cap no single %g can reach. */
static void trim_g_zeros(sink_t *b, size_t start) {
    size_t stored = b->len < b->cap ? b->len : b->cap;
    size_t mant = stored;
    for (size_t i = start; i < stored; i++) {
        if (b->buf[i] == 'e' || b->buf[i] == 'E') {
            mant = i;
            break;
        }
    }
    int has_dot = 0;
    for (size_t i = start; i < mant; i++) {
        if (b->buf[i] == '.') {
            has_dot = 1;
            break;
        }
    }
    if (!has_dot) {
        return;
    }
    size_t last = mant;
    while (last > start && b->buf[last - 1] == '0') {
        last--;
    }
    if (last > start && b->buf[last - 1] == '.') {
        last--;
    }
    if (last == mant) {
        return;
    }
    for (size_t i = mant; i < stored; i++) {
        b->buf[last + (i - mant)] = b->buf[i];
    }
    b->len = stored - (mant - last);
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

        /* M98: `#`, the fifth flag, and the one that was missing.
         *
         * Found by GCC compiling C++ ON this machine. dwarf2out.cc emits
         * `fprintf (asm_out_file, "\t.cfi_personality %#x,", enc)`, and
         * a formatter that does not know `#` fell through to the
         * unknown-conversion path, printed the three characters `%#x`
         * into the assembly, and produced a file the machine's own `as`
         * then refused. Twenty-eight times, in a compile that had
         * already run for fifteen minutes.
         *
         * Nothing in tests/printf/cases.tsv had a `#` in it, which is
         * why five bug classes were found there and this one was not:
         * the differential test is only as good as its case list, and
         * the case list is the part a person writes. */
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
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
        /* M98: `h` counted rather than skipped. Skipping was almost
         * right - promotion has already widened the argument - but %hx
         * of a negative short printed all eight bytes of the promotion
         * instead of the two the caller asked about. readelf prints ELF
         * half-words with PRIx16, which is how "almost" got caught. */
        int lng = 0, sht = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z') {
            if (*p == 'l' || *p == 'z') {
                lng++;
            } else {
                sht++;
            }
            p++;
        }

        /* Everything is formatted into `body` first so width can be
         * applied uniformly - a conversion that emitted directly would
         * need to know its own length twice. */
        char body[512];
        sink_t b = {body, sizeof(body), 0};
        char conv = *p;
        /* How many characters at the front of `body` are a prefix rather
         * than a digit - the "0x" of `%#x`. The zero-padding rule below
         * treats it exactly as it treats a sign: `%#010x` of 0xdead is
         * 0x0000dead, with the zeros AFTER the prefix. */
        int alt_prefix = 0;

        if (conv == 'd' || conv == 'i') {
            long long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            if (sht == 1)      v = (short)v;
            else if (sht >= 2) v = (signed char)v;
            unsigned long long mag = (unsigned long long)(v < 0 ? -v : v);
            char nbuf[24];
            /* Zero with an explicit zero precision converts to no
             * characters at all (C99) - the sign, if asked for, stays. */
            int nlen = (prec == 0 && v == 0) ? 0 : format_uint(mag, 10, 0, nbuf);
            if (v < 0) emit(&b, '-');
            else if (plus) emit(&b, '+');
            else if (space) emit(&b, ' ');
            if (prec > nlen) {
                emit_pad(&b, '0', prec - nlen);
            }
            emit_str(&b, nbuf, nlen);
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            unsigned long long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            if (sht == 1)      v = (unsigned short)v;
            else if (sht >= 2) v = (unsigned char)v;
            int base = conv == 'u' ? 10 : (conv == 'o' ? 8 : 16);
            char nbuf[24];
            int nlen = (prec == 0 && v == 0) ? 0 : format_uint(v, base, conv == 'X', nbuf);
            /* C99 7.19.6.1: `#` prefixes a NON-ZERO hexadecimal with
             * 0x/0X, and forces an octal to begin with a zero - by
             * raising the precision, which is why it is done here rather
             * than by emitting a character: `%#.3o` of 8 is 010, not
             * 0010. It means nothing for %u and is ignored there, as it
             * is for every conversion the standard does not name. */
            if (alt && (conv == 'x' || conv == 'X') && v != 0) {
                alt_prefix = 2;
            }
            if (alt && conv == 'o' && (nlen == 0 || nbuf[0] != '0') &&
                prec <= nlen) {
                prec = nlen + 1;
            }
            /* The 0 flag pads to the width AFTER the prefix, which
             * emit_pad below cannot know about - so the prefix is
             * emitted into the body and the width arithmetic downstream
             * counts it, exactly as it counts a sign. */
            if (alt_prefix) {
                emit_str(&b, conv == 'X' ? "0X" : "0x", 2);
            }
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
            emit_double(&b, va_arg(ap, double), prec, 0, conv == 'F', alt);
        } else if (conv == 'e' || conv == 'E') {
            emit_double(&b, va_arg(ap, double), prec, 1, conv == 'E', alt);
        } else if (conv == 'g' || conv == 'G') {
            /* %g's precision is SIGNIFICANT digits, not decimal places -
             * the standard's rule is: with exponent X and precision P,
             * print as %f with P-1-X decimals when -4 <= X < P, as %e
             * with P-1 decimals otherwise, then remove trailing zeros.
             * An earlier version did only the exponent test and its own
             * comment admitted the trim was "the half that changes what
             * a number *is*"; M98's printf test agreed, five times. */
            double v = va_arg(ap, double);
            double mag = v < 0 ? -v : v;
            int P = prec < 0 ? 6 : (prec == 0 ? 1 : prec);
            if (P > 17) {
                P = 17;
            }
            int X = 0;
            if (mag != 0.0 && !is_nan(mag) && !is_inf(mag)) {
                double m = mag;
                while (m >= 10.0) { m /= 10.0; X++; }
                while (m < 1.0)  { m *= 10.0; X--; }
                /* Rounding to P digits can carry into the next decade -
                 * 999999.9 at six digits is 1e+06, not a seven-digit %f.
                 * Decide the branch from the value rounding will print. */
                double half = 0.5;
                for (int hd = 1; hd < P; hd++) {
                    half /= 10.0;
                }
                if (m + half >= 10.0) {
                    X++;
                }
            }
            size_t start = b.len;
            if (X >= -4 && X < P) {
                emit_double(&b, v, P - 1 - X, 0, 0, alt);
            } else {
                emit_double(&b, v, P - 1, 1, conv == 'G', alt);
            }
            /* `#` on a %g means "keep the trailing zeros", which is the
             * one place the flag does something by NOT doing something.
             * C99 7.19.6.1: the trim is suppressed entirely. */
            if (!alt) {
                trim_g_zeros(&b, start);
            }
        } else if (conv == '\0') {
            break;
        } else {
            emit(&b, '%');
            emit(&b, conv);
        }

        int blen = (int)(b.len < sizeof(body) ? b.len : sizeof(body) - 1);
        int pad = width - blen;
        /* Two rules of the 0 flag, both found by M98's printf test: the
         * zeros go AFTER the sign (%05d of -42 is -0042, not 00-42),
         * and a precision turns the flag off for the integer
         * conversions but never for the floating ones (C99 7.19.6.1). */
        int isfloat = conv == 'f' || conv == 'F' || conv == 'e' ||
                      conv == 'E' || conv == 'g' || conv == 'G';
        int zpad = zero && !left && (prec < 0 || isfloat);
        /* A sign is one character; `%#x`'s prefix is two. Both are
         * "characters the zeros go after", which is the only property
         * this arithmetic cares about. */
        int keep = 0;
        if (zpad && blen > 0) {
            if (body[0] == '-' || body[0] == '+' || body[0] == ' ') {
                keep = 1;
            } else if (alt_prefix && blen >= 2) {
                keep = alt_prefix;
            }
        }
        if (pad > 0 && !left) {
            if (keep) {
                emit_str(&s, body, keep);
                emit_pad(&s, '0', pad);
                emit_str(&s, body + keep, blen - keep);
            } else {
                emit_pad(&s, zpad ? '0' : ' ', pad);
                emit_str(&s, body, blen);
            }
        } else {
            emit_str(&s, body, blen);
            if (pad > 0 && left) {
                emit_pad(&s, ' ', pad);
            }
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
        stream_write(f, line, (size_t)len);
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
            open_files[i].unget = -1;
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
    return f ? (size_t)f->wlen : 0; /* M98: there is a buffer now */
}

/* ---- M97: position as an opaque token, and two ways to open ----------
 *
 * fgetpos/fsetpos are ftell/fseek with the position wrapped in a type a
 * program cannot do arithmetic on. On this OS that is all they are,
 * because a position here IS a byte offset; the wrapper exists so that a
 * program written against the standard's opacity keeps working on a
 * system where it is not.
 */
int fgetpos(FILE *f, fpos_t *pos) {
    if (!f || !pos) {
        return -1;
    }
    long where = ftell(f);
    if (where < 0) {
        return -1;
    }
    pos->__pos = where;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos) {
    if (!f || !pos) {
        return -1;
    }
    return fseek(f, pos->__pos, SEEK_SET);
}

/* freopen: close whatever this stream was and reopen it on a new path,
 * KEEPING THE SAME FILE OBJECT. That last part is the whole point of the
 * call and the reason it cannot be written as fclose-then-fopen by the
 * caller: the standard streams are the usual target
 * (`freopen("out", "w", stdout)`), and every pointer to stdout in the
 * program - including ones inside a library it did not write - has to
 * keep working afterwards.
 *
 * A NULL path means "reopen the same file with a new mode", which this
 * OS cannot do: there is no way to ask a descriptor what path it came
 * from. Refused rather than silently ignored - a program that asked to
 * change a stream from read to write and was told it succeeded would
 * then write nothing, somewhere else.
 */
FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (!f || !path) {
        return (FILE *)0;
    }
    uint32_t flags = 0;
    for (const char *m = mode; m && *m; m++) {
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
    /* The old descriptor goes only once the new one is in hand. A
     * freopen that fails must leave the stream exactly as it was, and
     * closing first would leave it closed. */
    if (f->fd >= 0) {
        sys_close(f->fd);
    }
    f->fd = (int)fd;
    f->eof = 0;
    f->err = 0;
    f->unget = -1;
    f->used = 1;
    return f;
}

/* tmpfile: a stream on a file with no name a program can use.
 *
 * Made in /tmp with a name derived from the pid and a counter, opened,
 * and then UNLINKED while the descriptor is still open - so the file has
 * no directory entry from the moment this returns and its blocks come
 * back when the last descriptor closes. That is what makes it a
 * temporary file rather than a file in a temporary place, and it is the
 * one part of this a caller cannot arrange for itself.
 */
FILE *tmpfile(void) {
    static int counter;
    char name[64];
    long pid = sys_getpid();
    int n = 0;
    const char *dir = "/tmp/tmpf";
    while (dir[n] && n < 32) {
        name[n] = dir[n];
        n++;
    }
    long v = pid * 1000 + (++counter);
    char digits[16];
    int d = 0;
    do {
        digits[d++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v && d < 16);
    while (d > 0) {
        name[n++] = digits[--d];
    }
    name[n] = '\0';
    FILE *f = fopen(name, "w+");
    if (!f) {
        return (FILE *)0;
    }
    /* Unlinked while open. If this fails the stream still works and the
     * file is merely visible, which is worse than a temporary file and
     * better than no file - so it is not a reason to fail the call. */
    sys_unlink(name);
    return f;
}
