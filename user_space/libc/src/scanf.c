/* user_space/libc/src/scanf.c - M89
 *
 * The scanf family, over one engine that reads through a callback.
 *
 * **Why a callback rather than two implementations.** sscanf reads from
 * a string and fscanf from a stream, and the difference is exactly two
 * functions: "give me the next character" and "put one back". Everything
 * else - the width, the length modifier, the scanset, the conversion,
 * the whitespace rules - is identical, and a second copy of it would be
 * a second copy of every bug. So the engine takes a `src` with get/unget
 * and the two entry points differ only in what they hand it.
 *
 * The one place a stream is genuinely worse than a string: `unget` on a
 * stream is `ungetc`, which on this unbuffered stdio is an lseek
 * backwards, so it fails on a pipe. That means fscanf can misparse where
 * sscanf would not - specifically at a conversion that has to look one
 * character past what it consumes, like `%d` followed by a non-digit.
 * The engine therefore keeps ONE character of pushback of its own and
 * never calls ungetc, so a single-character lookahead works on any
 * source including a pipe. A conversion that needed two would not, and
 * none of the ones supported here does.
 *
 * See <stdio.h> for which conversions exist and which deliberately do
 * not.
 */
#include <stdio.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;   /* for sscanf; NULL for a stream */
    FILE *f;         /* for fscanf; NULL for a string */
    int pushback;    /* the one character of lookahead, or -1 */
    long consumed;   /* what %n reports */
} src_t;

static int src_get(src_t *src) {
    int c;
    if (src->pushback >= 0) {
        c = src->pushback;
        src->pushback = -1;
    } else if (src->s) {
        c = (unsigned char)*src->s;
        if (c == 0) {
            return EOF;
        }
        src->s++;
    } else {
        c = fgetc(src->f);
    }
    if (c != EOF) {
        src->consumed++;
    }
    return c;
}

static void src_unget(src_t *src, int c) {
    if (c == EOF) {
        return;
    }
    src->pushback = c;
    src->consumed--;
}

/* The length modifiers, as a size rather than as a set of flags: every
 * use of them here is "how wide is the object the pointer points at". */
enum { LEN_INT, LEN_CHAR, LEN_SHORT, LEN_LONG, LEN_LLONG, LEN_SIZE };

static void store_signed(void *p, int len, long long v) {
    switch (len) {
    case LEN_CHAR:  *(signed char *)p = (signed char)v; break;
    case LEN_SHORT: *(short *)p = (short)v; break;
    case LEN_LONG:  *(long *)p = (long)v; break;
    case LEN_LLONG: *(long long *)p = v; break;
    case LEN_SIZE:  *(size_t *)p = (size_t)v; break;
    default:        *(int *)p = (int)v; break;
    }
}

static void store_unsigned(void *p, int len, unsigned long long v) {
    switch (len) {
    case LEN_CHAR:  *(unsigned char *)p = (unsigned char)v; break;
    case LEN_SHORT: *(unsigned short *)p = (unsigned short)v; break;
    case LEN_LONG:  *(unsigned long *)p = (unsigned long)v; break;
    case LEN_LLONG: *(unsigned long long *)p = v; break;
    case LEN_SIZE:  *(size_t *)p = (size_t)v; break;
    default:        *(unsigned int *)p = (unsigned int)v; break;
    }
}

static int digit_value(int c, int base) {
    int v;
    if (c >= '0' && c <= '9') {
        v = c - '0';
    } else if (c >= 'a' && c <= 'z') {
        v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
        v = c - 'A' + 10;
    } else {
        return -1;
    }
    return v < base ? v : -1;
}

/* Reads an integer in `base` (0 means "look at the prefix"). Returns 0
 * if nothing that could be an integer was there, which is a matching
 * failure and stops the whole scan - the distinction POSIX draws between
 * a matching failure and an input failure. */
static int scan_int(src_t *src, int base, int width, int is_signed,
                    unsigned long long *out, int *negative) {
    int c;
    int any = 0;
    unsigned long long v = 0;
    *negative = 0;

    c = src_get(src);
    if (width > 0 && (c == '+' || c == '-')) {
        *negative = (c == '-');
        width--;
        c = src_get(src);
    }
    (void)is_signed;

    if (base == 0 || base == 16) {
        if (width > 0 && c == '0') {
            any = 1;
            width--;
            c = src_get(src);
            if (width > 0 && (c == 'x' || c == 'X')) {
                base = 16;
                width--;
                c = src_get(src);
                any = 0; /* "0x" with no digit after it is not a number */
            } else if (base == 0) {
                base = 8;
            }
        } else if (base == 0) {
            base = 10;
        }
    }

    while (width > 0) {
        int d = digit_value(c, base);
        if (d < 0) {
            break;
        }
        v = v * (unsigned long long)base + (unsigned long long)d;
        any = 1;
        width--;
        c = src_get(src);
    }
    src_unget(src, c);
    *out = v;
    return any;
}

static int scan_float(src_t *src, int width, long double *out) {
    /* Assembled into a small buffer and handed to strtod, so there is
     * exactly one float parser in this library rather than two that
     * round differently. A number longer than the buffer is truncated at
     * the buffer, which is a real limit and stated rather than hidden -
     * it is 63 significant characters, well past what a double holds. */
    char buf[64];
    int n = 0;
    int c = src_get(src);
    int seen_digit = 0;

    while (width > 0 && n < (int)sizeof(buf) - 1) {
        int keep = 0;
        if (c == '+' || c == '-') {
            keep = (n == 0) || (buf[n - 1] == 'e' || buf[n - 1] == 'E');
        } else if (c >= '0' && c <= '9') {
            keep = 1;
            seen_digit = 1;
        } else if (c == '.') {
            keep = 1;
        } else if ((c == 'e' || c == 'E') && seen_digit) {
            keep = 1;
        }
        if (!keep) {
            break;
        }
        buf[n++] = (char)c;
        width--;
        c = src_get(src);
    }
    src_unget(src, c);
    buf[n] = '\0';
    if (!seen_digit) {
        return 0;
    }
    char *end = 0;
    *out = (long double)strtod(buf, &end);
    return 1;
}

int vsscanf_src(src_t *src, const char *fmt, va_list ap);

/* ---- M89: the difference between "no match" and "nothing there" ------
 *
 * POSIX draws a line every hand-written scanf misses: a *matching*
 * failure - the input was there and did not look like what the format
 * asked for - returns the number of conversions assigned so far, and an
 * *input* failure - there was no input left at all - returns EOF when
 * nothing has been assigned yet.
 *
 * It matters because `while (sscanf(...) != EOF)` is how a program reads
 * until the end. This returned 0 for an empty input, so that loop never
 * terminated; the differential test against the host's own sscanf found
 * it on its first run, which is the entire argument for having one.
 *
 * The peek is safe at every call site: a failed conversion has already
 * pushed back whatever it looked at, so this looks at the same character
 * the next call would. */
static int fail_return(src_t *src, int assigned) {
    if (assigned == 0) {
        int c = src_get(src);
        if (c == EOF) {
            return EOF;
        }
        src_unget(src, c);
    }
    return assigned;
}

int vsscanf_src(src_t *src, const char *fmt, va_list ap) {
    int assigned = 0;

    for (const char *p = fmt; *p; p++) {
        if (isspace((unsigned char)*p)) {
            /* Whitespace in the format matches any run of it, including
             * none. */
            int c;
            do {
                c = src_get(src);
            } while (c != EOF && isspace(c));
            src_unget(src, c);
            continue;
        }
        if (*p != '%') {
            int c = src_get(src);
            if (c != (unsigned char)*p) {
                src_unget(src, c);
                return fail_return(src, assigned); /* a literal that did not match */
            }
            continue;
        }

        p++;
        if (*p == '%') {
            int c = src_get(src);
            if (c != '%') {
                src_unget(src, c);
                return fail_return(src, assigned);
            }
            continue;
        }

        int suppress = 0;
        if (*p == '*') {
            suppress = 1;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        if (width == 0) {
            width = 0x7fffffff;
        }
        int len = LEN_INT;
        if (*p == 'h') {
            p++;
            len = LEN_SHORT;
            if (*p == 'h') {
                p++;
                len = LEN_CHAR;
            }
        } else if (*p == 'l') {
            p++;
            len = LEN_LONG;
            if (*p == 'l') {
                p++;
                len = LEN_LLONG;
            }
        } else if (*p == 'z') {
            p++;
            len = LEN_SIZE;
        } else if (*p == 'j' || *p == 't') {
            p++;
            len = LEN_LLONG;
        }

        int conv = (unsigned char)*p;
        if (conv == '\0') {
            return assigned; /* the format ran out - not a failure */
        }

        /* Every conversion but %c, %[ and %n skips leading whitespace
         * first. That asymmetry is the standard's and it is the thing
         * most hand-written scanfs get wrong. */
        if (conv != 'c' && conv != '[' && conv != 'n') {
            int c;
            do {
                c = src_get(src);
            } while (c != EOF && isspace(c));
            src_unget(src, c);
        }

        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            int base = (conv == 'o') ? 8
                       : (conv == 'x' || conv == 'X') ? 16
                       : (conv == 'i') ? 0
                                       : 10;
            unsigned long long v = 0;
            int neg = 0;
            if (!scan_int(src, base, width, conv == 'd' || conv == 'i', &v,
                          &neg)) {
                return fail_return(src, assigned);
            }
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                if (conv == 'd' || conv == 'i') {
                    long long sv = neg ? -(long long)v : (long long)v;
                    store_signed(dst, len, sv);
                } else {
                    store_unsigned(dst, len, neg ? (unsigned long long)0 - v : v);
                }
                assigned++;
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            long double v = 0;
            if (!scan_float(src, width, &v)) {
                return fail_return(src, assigned);
            }
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                if (len == LEN_LONG || len == LEN_LLONG) {
                    *(double *)dst = (double)v;
                } else {
                    *(float *)dst = (float)v;
                }
                assigned++;
            }
            break;
        }
        case 'c': {
            /* No leading-whitespace skip, and a default width of 1
             * rather than "as many as there are" - both are what makes
             * %c different from %s. */
            int want = (width == 0x7fffffff) ? 1 : width;
            char *dst = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            while (got < want) {
                int c = src_get(src);
                if (c == EOF) {
                    break;
                }
                if (dst) {
                    dst[got] = (char)c;
                }
                got++;
            }
            if (got < want) {
                return fail_return(src, assigned);
            }
            if (dst) {
                assigned++;
            }
            break;
        }
        case 's': {
            char *dst = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            int c = src_get(src);
            while (c != EOF && !isspace(c) && got < width) {
                if (dst) {
                    dst[got] = (char)c;
                }
                got++;
                c = src_get(src);
            }
            src_unget(src, c);
            if (got == 0) {
                return fail_return(src, assigned);
            }
            if (dst) {
                dst[got] = '\0';
                assigned++;
            }
            break;
        }
        case '[': {
            /* A scanset. `^` inverts, a `]` immediately after either one
             * is a literal `]`, and `a-z` is a range - the three rules
             * that make this parseable at all. */
            unsigned char set[256];
            memset(set, 0, sizeof(set));
            p++;
            int invert = 0;
            if (*p == '^') {
                invert = 1;
                p++;
            }
            if (*p == ']') {
                set[(unsigned char)']'] = 1;
                p++;
            }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    for (unsigned char c = (unsigned char)p[0];
                         c <= (unsigned char)p[2]; c++) {
                        set[c] = 1;
                    }
                    p += 3;
                } else {
                    set[(unsigned char)*p] = 1;
                    p++;
                }
            }
            if (*p != ']') {
                return assigned; /* unterminated scanset - the format is wrong */
            }
            char *dst = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            int c = src_get(src);
            while (c != EOF && got < width &&
                   (set[(unsigned char)c] ? !invert : invert)) {
                if (dst) {
                    dst[got] = (char)c;
                }
                got++;
                c = src_get(src);
            }
            src_unget(src, c);
            if (got == 0) {
                return fail_return(src, assigned);
            }
            if (dst) {
                dst[got] = '\0';
                assigned++;
            }
            break;
        }
        case 'n': {
            /* Not an assignment, and therefore not counted - which is
             * the rule that makes `sscanf(s, "%d%n", &v, &used)` return
             * 1 rather than 2. */
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                store_signed(dst, len, src->consumed);
            }
            break;
        }
        default:
            /* An unsupported conversion stops the scan rather than being
             * skipped - see <stdio.h>. A skip would assign the next
             * argument from the wrong conversion, which is worse. */
            return assigned;
        }
    }
    return assigned;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    if (!str || !fmt) {
        return EOF;
    }
    src_t src = {str, 0, -1, 0};
    return vsscanf_src(&src, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(str, fmt, ap);
    va_end(ap);
    return n;
}

int fscanf(FILE *f, const char *fmt, ...) {
    if (!f || !fmt) {
        return EOF;
    }
    src_t src = {0, f, -1, 0};
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf_src(&src, fmt, ap);
    va_end(ap);
    /* The one character of lookahead is given back to the stream, so a
     * caller that mixes fscanf with fgetc sees it. On a pipe this fails
     * and the character is lost - see the file header, which is the
     * honest statement of the limit rather than a silent one. */
    if (src.pushback >= 0) {
        ungetc(src.pushback, f);
    }
    return n;
}

int scanf(const char *fmt, ...) {
    src_t src = {0, stdin, -1, 0};
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf_src(&src, fmt, ap);
    va_end(ap);
    if (src.pushback >= 0) {
        ungetc(src.pushback, stdin);
    }
    return n;
}
