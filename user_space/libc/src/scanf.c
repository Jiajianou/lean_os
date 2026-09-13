#include <stdio.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    FILE *f;
    int pushback;
    long consumed;
} source_t;

static int source_get(source_t *source) {
    int c;
    if (source->pushback >= 0) {
        c = source->pushback;
        source->pushback = -1;
    } else if (source->s) {
        c = (unsigned char)*source->s;
        if (c == 0) {
            return EOF;
        }
        source->s++;
    } else {
        c = fgetc(source->f);
    }
    if (c != EOF) {
        source->consumed++;
    }
    return c;
}

static void source_unget(source_t *source, int c) {
    if (c == EOF) {
        return;
    }
    source->pushback = c;
    source->consumed--;
}

enum { LENGTH_INT, LENGTH_CHAR, LENGTH_SHORT, LENGTH_LONG, LENGTH_LLONG, LENGTH_SIZE };

static void store_signed(void *p, int length, long long v) {
    switch (length) {
    case LENGTH_CHAR:  *(signed char *)p = (signed char)v; break;
    case LENGTH_SHORT: *(short *)p = (short)v; break;
    case LENGTH_LONG:  *(long *)p = (long)v; break;
    case LENGTH_LLONG: *(long long *)p = v; break;
    case LENGTH_SIZE:  *(size_t *)p = (size_t)v; break;
    default:        *(int *)p = (int)v; break;
    }
}

static void store_unsigned(void *p, int length, unsigned long long v) {
    switch (length) {
    case LENGTH_CHAR:  *(unsigned char *)p = (unsigned char)v; break;
    case LENGTH_SHORT: *(unsigned short *)p = (unsigned short)v; break;
    case LENGTH_LONG:  *(unsigned long *)p = (unsigned long)v; break;
    case LENGTH_LLONG: *(unsigned long long *)p = v; break;
    case LENGTH_SIZE:  *(size_t *)p = (size_t)v; break;
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

static int scan_int(source_t *source, int base, int width, int is_signed,
                    unsigned long long *out, int *negative) {
    int c;
    int any = 0;
    unsigned long long v = 0;
    *negative = 0;

    c = source_get(source);
    if (width > 0 && (c == '+' || c == '-')) {
        *negative = (c == '-');
        width--;
        c = source_get(source);
    }
    (void)is_signed;

    if (base == 0 || base == 16) {
        if (width > 0 && c == '0') {
            any = 1;
            width--;
            c = source_get(source);
            if (width > 0 && (c == 'x' || c == 'X')) {
                base = 16;
                width--;
                c = source_get(source);
                any = 0;
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
        c = source_get(source);
    }
    source_unget(source, c);
    *out = v;
    return any;
}

static int scan_float(source_t *source, int width, long double *out) {
    char buffer[64];
    int n = 0;
    int c = source_get(source);
    int seen_digit = 0;

    while (width > 0 && n < (int)sizeof(buffer) - 1) {
        int keep = 0;
        if (c == '+' || c == '-') {
            keep = (n == 0) || (buffer[n - 1] == 'e' || buffer[n - 1] == 'E');
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
        buffer[n++] = (char)c;
        width--;
        c = source_get(source);
    }
    source_unget(source, c);
    buffer[n] = '\0';
    if (!seen_digit) {
        return 0;
    }
    char *end = 0;
    *out = (long double)strtod(buffer, &end);
    return 1;
}

int vsscanf_source(source_t *source, const char *fmt, va_list ap);

static int fail_return(source_t *source, int assigned) {
    if (assigned == 0) {
        int c = source_get(source);
        if (c == EOF) {
            return EOF;
        }
        source_unget(source, c);
    }
    return assigned;
}

int vsscanf_source(source_t *source, const char *fmt, va_list ap) {
    int assigned = 0;

    for (const char *p = fmt; *p; p++) {
        if (isspace((unsigned char)*p)) {
            int c;
            do {
                c = source_get(source);
            } while (c != EOF && isspace(c));
            source_unget(source, c);
            continue;
        }
        if (*p != '%') {
            int c = source_get(source);
            if (c != (unsigned char)*p) {
                source_unget(source, c);
                return fail_return(source, assigned);
            }
            continue;
        }

        p++;
        if (*p == '%') {
            int c = source_get(source);
            if (c != '%') {
                source_unget(source, c);
                return fail_return(source, assigned);
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
        int length = LENGTH_INT;
        if (*p == 'h') {
            p++;
            length = LENGTH_SHORT;
            if (*p == 'h') {
                p++;
                length = LENGTH_CHAR;
            }
        } else if (*p == 'l') {
            p++;
            length = LENGTH_LONG;
            if (*p == 'l') {
                p++;
                length = LENGTH_LLONG;
            }
        } else if (*p == 'z') {
            p++;
            length = LENGTH_SIZE;
        } else if (*p == 'j' || *p == 't') {
            p++;
            length = LENGTH_LLONG;
        }

        int conv = (unsigned char)*p;
        if (conv == '\0') {
            return assigned;
        }

        if (conv != 'c' && conv != '[' && conv != 'n') {
            int c;
            do {
                c = source_get(source);
            } while (c != EOF && isspace(c));
            source_unget(source, c);
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
            if (!scan_int(source, base, width, conv == 'd' || conv == 'i', &v,
                          &neg)) {
                return fail_return(source, assigned);
            }
            if (!suppress) {
                void *destination = va_arg(ap, void *);
                if (conv == 'd' || conv == 'i') {
                    long long sv = neg ? -(long long)v : (long long)v;
                    store_signed(destination, length, sv);
                } else {
                    store_unsigned(destination, length, neg ? (unsigned long long)0 - v : v);
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
            if (!scan_float(source, width, &v)) {
                return fail_return(source, assigned);
            }
            if (!suppress) {
                void *destination = va_arg(ap, void *);
                if (length == LENGTH_LONG || length == LENGTH_LLONG) {
                    *(double *)destination = (double)v;
                } else {
                    *(float *)destination = (float)v;
                }
                assigned++;
            }
            break;
        }
        case 'c': {
            int want = (width == 0x7fffffff) ? 1 : width;
            char *destination = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            while (got < want) {
                int c = source_get(source);
                if (c == EOF) {
                    break;
                }
                if (destination) {
                    destination[got] = (char)c;
                }
                got++;
            }
            if (got < want) {
                return fail_return(source, assigned);
            }
            if (destination) {
                assigned++;
            }
            break;
        }
        case 's': {
            char *destination = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            int c = source_get(source);
            while (c != EOF && !isspace(c) && got < width) {
                if (destination) {
                    destination[got] = (char)c;
                }
                got++;
                c = source_get(source);
            }
            source_unget(source, c);
            if (got == 0) {
                return fail_return(source, assigned);
            }
            if (destination) {
                destination[got] = '\0';
                assigned++;
            }
            break;
        }
        case '[': {
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
                return assigned;
            }
            char *destination = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            int c = source_get(source);
            while (c != EOF && got < width &&
                   (set[(unsigned char)c] ? !invert : invert)) {
                if (destination) {
                    destination[got] = (char)c;
                }
                got++;
                c = source_get(source);
            }
            source_unget(source, c);
            if (got == 0) {
                return fail_return(source, assigned);
            }
            if (destination) {
                destination[got] = '\0';
                assigned++;
            }
            break;
        }
        case 'n': {
            if (!suppress) {
                void *destination = va_arg(ap, void *);
                store_signed(destination, length, source->consumed);
            }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

int vsscanf(const char *string, const char *fmt, va_list ap) {
    if (!string || !fmt) {
        return EOF;
    }
    source_t source = {string, 0, -1, 0};
    return vsscanf_source(&source, fmt, ap);
}

int sscanf(const char *string, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(string, fmt, ap);
    va_end(ap);
    return n;
}

int fscanf(FILE *f, const char *fmt, ...) {
    if (!f || !fmt) {
        return EOF;
    }
    source_t source = {0, f, -1, 0};
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf_source(&source, fmt, ap);
    va_end(ap);
    if (source.pushback >= 0) {
        ungetc(source.pushback, f);
    }
    return n;
}

int scanf(const char *fmt, ...) {
    source_t source = {0, stdin, -1, 0};
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf_source(&source, fmt, ap);
    va_end(ap);
    if (source.pushback >= 0) {
        ungetc(source.pushback, stdin);
    }
    return n;
}
