#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "syscall_wrappers.h"

struct FILE {
    int fd;
    int eof;
    int used;
    int err;
    int unget;
    wint_t wunget;
    char wbuf[BUFSIZ];
    int wlen;
    int mode;
};

static FILE std_files[3] = {
    {.fd = 0, .eof = 0, .used = 1, .err = 0, .unget = -1,
     .wunget = (wint_t)-1, .wbuf = {0}, .wlen = 0, .mode = _IOLBF},
    {.fd = 1, .eof = 0, .used = 1, .err = 0, .unget = -1,
     .wunget = (wint_t)-1, .wbuf = {0}, .wlen = 0, .mode = _IOLBF},
    {.fd = 2, .eof = 0, .used = 1, .err = 0, .unget = -1,
     .wunget = (wint_t)-1, .wbuf = {0}, .wlen = 0, .mode = _IOLBF},
};
FILE *stdin = &std_files[0];
FILE *stdout = &std_files[1];
static FILE stderr_file = {.fd = 1, .eof = 0, .used = 1, .err = 0,
                           .unget = -1, .wunget = (wint_t)-1, .wbuf = {0},
                           .wlen = 0, .mode = _IONBF};
FILE *stderr = &stderr_file;

#define FOPEN_MAX_FILES 16
static FILE open_files[FOPEN_MAX_FILES];

static int stream_flush(FILE *f) {
    if (!f || f->wlen == 0) {
        return 0;
    }
    int len = f->wlen;
    f->wlen = 0;
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
        errno = EINVAL;
        return (FILE *)0;
    }
    long fd = sys_open(path, flags);
    if (fd < 0) {
        errno = __lean_path_errno(path, (flags & OPEN_CREATE) != 0);
        return (FILE *)0;
    }
    for (int i = 0; i < FOPEN_MAX_FILES; i++) {
        if (!open_files[i].used) {
            open_files[i].fd = (int)fd;
            open_files[i].eof = 0;
            open_files[i].err = 0;
            open_files[i].unget = -1;
            open_files[i].wlen = 0;
            open_files[i].mode = _IOFBF;
            open_files[i].used = 1;
            return &open_files[i];
        }
    }
    sys_close((int)fd);
    errno = EMFILE;
    return (FILE *)0;
}

int fclose(FILE *f) {
    if (!f || !f->used) {
        return EOF;
    }
    stream_flush(f);
    sys_close(f->fd);
    if (f >= open_files && f < open_files + FOPEN_MAX_FILES) {
        f->used = 0;
    }
    return 0;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f) {
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
    stream_flush(f);
    if (f->unget >= 0) {
        if (whence == SEEK_CUR) {
            offset -= 1;
        }
        f->unget = -1;
    }
    f->wunget = (wint_t)-1;
    return sys_lseek(f->fd, offset, whence) >= 0 ? 0 : -1;
}

long ftell(FILE *f) {
    if (!f) {
        return -1;
    }
    stream_flush(f);
    long pos = sys_lseek(f->fd, 0, SEEK_CUR);
    if (pos > 0 && f->unget >= 0) {
        pos -= 1;
    }
    return pos;
}

int fflush(FILE *f) {
    if (!f) {
        __lean_stdio_flush_all();
        return 0;
    }
    return stream_flush(f);
}

int feof(FILE *f) {
    return f ? f->eof : 1;
}

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
        f->wunget = (wint_t)-1;
    }
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)buf;
    (void)size;
    if (!f || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) {
        return -1;
    }
    stream_flush(f);
    f->mode = mode;
    return 0;
}

void setbuf(FILE *f, char *buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

wint_t fgetwc(FILE *f) {
    if (!f) {
        return WEOF;
    }
    if (f->wunget != (wint_t)-1) {
        wint_t c = f->wunget;
        f->wunget = (wint_t)-1;
        return c;
    }
    mbstate_t st;
    for (size_t i = 0; i < sizeof(st); i++) {
        ((unsigned char *)&st)[i] = 0;
    }
    for (int i = 0; i < 4; i++) {
        int b = fgetc(f);
        if (b == EOF) {
            if (i > 0) {
                errno = EILSEQ;
            }
            return WEOF;
        }
        char byte = (char)b;
        wchar_t wc = 0;
        size_t r = mbrtowc(&wc, &byte, 1, &st);
        if (r == (size_t)-1) {
            errno = EILSEQ;
            return WEOF;
        }
        if (r == (size_t)-2) {
            continue;
        }
        return (wint_t)wc;
    }
    errno = EILSEQ;
    return WEOF;
}

wint_t ungetwc(wint_t c, FILE *f) {
    if (!f || c == WEOF) {
        return WEOF;
    }
    if (f->wunget != (wint_t)-1) {
        return WEOF;
    }
    f->wunget = c;
    f->eof = 0;
    return c;
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) {
        return EOF;
    }
    if (f->unget >= 0) {
        return EOF;
    }
    f->unget = (unsigned char)c;
    f->eof = 0;
    return (unsigned char)c;
}

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
    stream_flush(f);
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

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
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

static double two_prod_err(double a, double b, double p) {
    const double split = 134217729.0;
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
        return d != 0;
    }

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
        prec = 17;
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
        emit(s, '.');
    }
    if (sci) {
        emit(s, upper ? 'E' : 'e');
        emit(s, exp10 < 0 ? '-' : '+');
        int e = exp10 < 0 ? -exp10 : exp10;
        char ebuf[8];
        int elen = format_uint((unsigned long long)e, 10, 0, ebuf);
        if (elen < 2) {
            emit(s, '0');
        }
        emit_str(s, ebuf, elen);
    }
}

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
        int lng = 0, sht = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z') {
            if (*p == 'l' || *p == 'z') {
                lng++;
            } else {
                sht++;
            }
            p++;
        }

        char body[512];
        sink_t b = {body, sizeof(body), 0};
        char conv = *p;
        int alt_prefix = 0;

        if (conv == 'd' || conv == 'i') {
            long long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            if (sht == 1)      v = (short)v;
            else if (sht >= 2) v = (signed char)v;
            unsigned long long mag = (unsigned long long)(v < 0 ? -v : v);
            char nbuf[24];
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
            if (alt && (conv == 'x' || conv == 'X') && v != 0) {
                alt_prefix = 2;
            }
            if (alt && conv == 'o' && (nlen == 0 || nbuf[0] != '0') &&
                prec <= nlen) {
                prec = nlen + 1;
            }
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
        int isfloat = conv == 'f' || conv == 'F' || conv == 'e' ||
                      conv == 'E' || conv == 'g' || conv == 'G';
        int zpad = zero && !left && (prec < 0 || isfloat);
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

int vasprintf(char **out, const char *fmt, va_list ap) {
    if (!out) {
        return -1;
    }
    va_list measure;
    va_copy(measure, ap);
    int needed = vsnprintf((char *)0, 0, fmt, measure);
    va_end(measure);
    if (needed < 0) {
        *out = (char *)0;
        return -1;
    }
    char *buf = (char *)malloc((size_t)needed + 1);
    if (!buf) {
        *out = (char *)0;
        return -1;
    }
    int written = vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    if (written < 0) {
        free(buf);
        *out = (char *)0;
        return -1;
    }
    *out = buf;
    return written;
}

int asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

void __assert_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "assertion failed: %s at %s:%d\n", expr, file, line);
    uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
    uintptr_t floor = (uintptr_t)&fp;
    for (int i = 0; i < 32; i++) {
        uintptr_t here = (uintptr_t)fp;
        if (here <= floor || here - floor > (8u << 20) || (here & 7u) != 0) {
            break;
        }
        fprintf(stderr, "  #%d %p\n", i, (void *)fp[1]);
        uintptr_t *next = (uintptr_t *)fp[0];
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    abort();
}

int getc(FILE *f) {
    return fgetc(f);
}

int putc(int c, FILE *f) {
    return fputc(c, f);
}

int getchar(void) {
    return fgetc(stdin);
}

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd < 0) {
        return (FILE *)0;
    }
    for (int i = 0; i < FOPEN_MAX_FILES; i++) {
        if (!open_files[i].used) {
            open_files[i].fd = fd;
            open_files[i].eof = 0;
            open_files[i].err = 0;
            open_files[i].unget = -1;
            open_files[i].wlen = 0;
            open_files[i].mode = _IOFBF;
            open_files[i].used = 1;
            return &open_files[i];
        }
    }
    return (FILE *)0;
}

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
                return -1;
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
    return f ? (size_t)f->wlen : 0;
}

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
    if (f->fd >= 0) {
        sys_close(f->fd);
    }
    f->fd = (int)fd;
    f->eof = 0;
    f->err = 0;
    f->unget = -1;
    f->wunget = (wint_t)-1;
    f->used = 1;
    return f;
}

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
    sys_unlink(name);
    return f;
}
