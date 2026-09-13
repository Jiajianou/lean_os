#include <wchar.h>

#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>

size_t wcslen(const wchar_t *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src) {
    wchar_t *out = dst;
    while ((*dst++ = *src++) != 0) {
    }
    return out;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src) {
    wchar_t *out = dst;
    while (*dst) {
        dst++;
    }
    while ((*dst++ = *src++) != 0) {
    }
    return out;
}

int wcscmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)((unsigned long)*a - (unsigned long)*b);
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) {
            return (int)((unsigned long)a[i] - (unsigned long)b[i]);
        }
    }
    return 0;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    for (; *s; s++) {
        if (*s == c) {
            return (wchar_t *)s;
        }
    }
    return c ? (wchar_t *)0 : (wchar_t *)s;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *found = 0;
    for (;; s++) {
        if (*s == c) {
            found = s;
        }
        if (!*s) {
            break;
        }
    }
    return (wchar_t *)found;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    return dst;
}

wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n) {
    if (dst < src) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            dst[i - 1] = src[i - 1];
        }
    }
    return dst;
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        s[i] = c;
    }
    return s;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)((unsigned long)a[i] - (unsigned long)b[i]);
        }
    }
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c) {
            return (wchar_t *)&s[i];
        }
    }
    return (wchar_t *)0;
}

static int wcs_in_set(wchar_t c, const wchar_t *set) {
    for (; *set; set++) {
        if (*set == c) {
            return 1;
        }
    }
    return 0;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept) {
    size_t n = 0;
    for (; s[n] && wcs_in_set(s[n], accept); n++) {
    }
    return n;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject) {
    size_t n = 0;
    for (; s[n] && !wcs_in_set(s[n], reject); n++) {
    }
    return n;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) {
    for (; *s; s++) {
        if (wcs_in_set(*s, accept)) {
            return (wchar_t *)s;
        }
    }
    return (wchar_t *)0;
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle) {
    if (!*needle) {
        return (wchar_t *)haystack;
    }
    for (; *haystack; haystack++) {
        size_t i = 0;
        while (needle[i] && haystack[i] == needle[i]) {
            i++;
        }
        if (!needle[i]) {
            return (wchar_t *)haystack;
        }
    }
    return (wchar_t *)0;
}

wchar_t *wcsdup(const wchar_t *s) {
    size_t n = wcslen(s) + 1;
    wchar_t *out = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!out) {
        return (wchar_t *)0;
    }
    wmemcpy(out, s, n);
    return out;
}

int wcscoll(const wchar_t *a, const wchar_t *b) {
    return wcscmp(a, b);
}

size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n) {
    size_t len = wcslen(src);
    if (n > 0) {
        size_t copy = len < n - 1 ? len : n - 1;
        wmemcpy(dst, src, copy);
        dst[copy] = 0;
    }
    return len;
}

static unsigned long wcs_to_ul(const wchar_t *s, wchar_t **end, int base,
                               int *neg, int *overflow) {
    const wchar_t *p = s;
    while (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r') {
        p++;
    }
    *neg = 0;
    if (*p == L'+') {
        p++;
    } else if (*p == L'-') {
        *neg = 1;
        p++;
    }
    const wchar_t *after_zero = (const wchar_t *)0;
    if ((base == 0 || base == 16) && p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) {
        after_zero = p + 1;
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == L'0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long acc = 0;
    const wchar_t *digits = p;
    *overflow = 0;
    for (; *p; p++) {
        int d;
        if (*p >= L'0' && *p <= L'9') {
            d = (int)(*p - L'0');
        } else if (*p >= L'a' && *p <= L'z') {
            d = (int)(*p - L'a') + 10;
        } else if (*p >= L'A' && *p <= L'Z') {
            d = (int)(*p - L'A') + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        if (!*overflow) {
            unsigned long limit = ULONG_MAX / (unsigned long)base;
            if (acc > limit ||
                (acc == limit &&
                 (unsigned long)d > ULONG_MAX - limit * (unsigned long)base)) {
                *overflow = 1;
            } else {
                acc = acc * (unsigned long)base + (unsigned long)d;
            }
        }
    }
    if (end) {
        if (p != digits) {
            *end = (wchar_t *)p;
        } else if (after_zero) {
            *end = (wchar_t *)after_zero;
        } else {
            *end = (wchar_t *)s;
        }
    }
    return acc;
}

long wcstol(const wchar_t *s, wchar_t **end, int base) {
    int neg = 0;
    int overflow = 0;
    unsigned long v = wcs_to_ul(s, end, base, &neg, &overflow);
    unsigned long max_mag = neg ? (unsigned long)LONG_MAX + 1UL
                                : (unsigned long)LONG_MAX;
    if (overflow || v > max_mag) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? (long)(0UL - v) : (long)v;
}

unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base) {
    int neg = 0;
    int overflow = 0;
    unsigned long v = wcs_to_ul(s, end, base, &neg, &overflow);
    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return neg ? (0UL - v) : v;
}

long long wcstoll(const wchar_t *s, wchar_t **end, int base) {
    _Static_assert(sizeof(long) == sizeof(long long),
                   "wcstoll widens wcstol; that is only exact on LP64");
    return (long long)wcstol(s, end, base);
}

unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base) {
    _Static_assert(sizeof(unsigned long) == sizeof(unsigned long long),
                   "wcstoull widens wcstoul; that is only exact on LP64");
    return (unsigned long long)wcstoul(s, end, base);
}

#define WCSTOD_MAX 128

static int wcs_numeric_byte(wchar_t c) {
    if (c >= L'0' && c <= L'9') {
        return 1;
    }
    if (c >= L'a' && c <= L'z') {
        return 1;
    }
    if (c >= L'A' && c <= L'Z') {
        return 1;
    }
    return c == L'+' || c == L'-' || c == L'.' || c == L' ' ||
           c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
}

static size_t wcs_narrow_number(const wchar_t *s, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap && s[n] && wcs_numeric_byte(s[n])) {
        buf[n] = (char)s[n];
        n++;
    }
    buf[n] = '\0';
    return n;
}

double wcstod(const wchar_t *s, wchar_t **end) {
    char buf[WCSTOD_MAX];
    size_t taken = wcs_narrow_number(s, buf, sizeof(buf));
    char *nend = buf;
    double v = strtod(buf, &nend);
    if (end) {
        *end = (wchar_t *)(s + (size_t)(nend - buf));
    }
    (void)taken;
    return v;
}

float wcstof(const wchar_t *s, wchar_t **end) {
    char buf[WCSTOD_MAX];
    wcs_narrow_number(s, buf, sizeof(buf));
    char *nend = buf;
    float v = strtof(buf, &nend);
    if (end) {
        *end = (wchar_t *)(s + (size_t)(nend - buf));
    }
    return v;
}

long double wcstold(const wchar_t *s, wchar_t **end) {
    char buf[WCSTOD_MAX];
    wcs_narrow_number(s, buf, sizeof(buf));
    char *nend = buf;
    long double v = strtold(buf, &nend);
    if (end) {
        *end = (wchar_t *)(s + (size_t)(nend - buf));
    }
    return v;
}

#define UTF8_INCOMPLETE ((size_t)-2)
#define UTF8_INVALID    ((size_t)-1)

static unsigned int utf8_min_for(unsigned char total) {
    switch (total) {
    case 1:  return 0x80u;
    case 2:  return 0x800u;
    default: return 0x10000u;
    }
}

static int utf8_is_valid(unsigned int wc, unsigned char total) {
    if (wc < utf8_min_for(total)) {
        return 0;
    }
    if (wc >= 0xD800u && wc <= 0xDFFFu) {
        return 0;
    }
    return wc <= 0x10FFFFu;
}

static size_t utf8_decode(wchar_t *out, const char *src, size_t n, mbstate_t *st) {
    size_t used = 0;
    while (used < n) {
        unsigned char b = (unsigned char)src[used];
        if (st->owed == 0) {
            used++;
            if (b < 0x80u) {
                if (out) {
                    *out = (wchar_t)b;
                }
                return used;
            }
            if (b < 0xC2u) {
                errno = EILSEQ;
                return UTF8_INVALID;
            }
            if (b < 0xE0u) {
                st->wc = b & 0x1Fu;
                st->owed = st->total = 1;
            } else if (b < 0xF0u) {
                st->wc = b & 0x0Fu;
                st->owed = st->total = 2;
            } else if (b < 0xF5u) {
                st->wc = b & 0x07u;
                st->owed = st->total = 3;
            } else {
                errno = EILSEQ;
                return UTF8_INVALID;
            }
            continue;
        }
        if ((b & 0xC0u) != 0x80u) {
            st->owed = st->total = 0;
            errno = EILSEQ;
            return UTF8_INVALID;
        }
        used++;
        st->wc = (st->wc << 6) | (b & 0x3Fu);
        if (--st->owed == 0) {
            unsigned int wc = st->wc;
            unsigned char total = st->total;
            st->wc = 0;
            st->total = 0;
            if (!utf8_is_valid(wc, total)) {
                errno = EILSEQ;
                return UTF8_INVALID;
            }
            if (out) {
                *out = (wchar_t)wc;
            }
            return used;
        }
    }
    return UTF8_INCOMPLETE;
}

static size_t utf8_encode(char *dst, wchar_t c) {
    unsigned int wc = (unsigned int)c;
    if (wc >= 0xD800u && wc <= 0xDFFFu) {
        errno = EILSEQ;
        return UTF8_INVALID;
    }
    if (wc < 0x80u) {
        dst[0] = (char)wc;
        return 1;
    }
    if (wc < 0x800u) {
        dst[0] = (char)(0xC0u | (wc >> 6));
        dst[1] = (char)(0x80u | (wc & 0x3Fu));
        return 2;
    }
    if (wc < 0x10000u) {
        dst[0] = (char)(0xE0u | (wc >> 12));
        dst[1] = (char)(0x80u | ((wc >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (wc & 0x3Fu));
        return 3;
    }
    if (wc <= 0x10FFFFu) {
        dst[0] = (char)(0xF0u | (wc >> 18));
        dst[1] = (char)(0x80u | ((wc >> 12) & 0x3Fu));
        dst[2] = (char)(0x80u | ((wc >> 6) & 0x3Fu));
        dst[3] = (char)(0x80u | (wc & 0x3Fu));
        return 4;
    }
    errno = EILSEQ;
    return UTF8_INVALID;
}

static mbstate_t internal_state;

int mbsinit(const mbstate_t *ps) {
    return !ps || ps->owed == 0;
}

size_t mbrtowc(wchar_t *dst, const char *src, size_t n, mbstate_t *ps) {
    mbstate_t *st = ps ? ps : &internal_state;
    if (!src) {
        st->wc = 0;
        st->owed = 0;
        st->total = 0;
        return 0;
    }
    wchar_t wc = 0;
    size_t r = utf8_decode(&wc, src, n, st);
    if (r == UTF8_INVALID || r == UTF8_INCOMPLETE) {
        return r;
    }
    if (dst) {
        *dst = wc;
    }
    return wc == 0 ? 0 : r;
}

size_t wcrtomb(char *dst, wchar_t c, mbstate_t *ps) {
    (void)ps;
    char scratch[4];
    if (!dst) {
        return 1;
    }
    size_t r = utf8_encode(scratch, c);
    if (r == UTF8_INVALID) {
        return r;
    }
    for (size_t i = 0; i < r; i++) {
        dst[i] = scratch[i];
    }
    return r;
}

int mbtowc(wchar_t *dst, const char *src, size_t n) {
    if (!src) {
        return 0;
    }
    mbstate_t st = {0, 0, 0};
    wchar_t wc = 0;
    size_t r = utf8_decode(&wc, src, n, &st);
    if (r == UTF8_INVALID || r == UTF8_INCOMPLETE) {
        errno = EILSEQ;
        return -1;
    }
    if (dst) {
        *dst = wc;
    }
    return wc == 0 ? 0 : (int)r;
}

int wctomb(char *dst, wchar_t c) {
    if (!dst) {
        return 0;
    }
    size_t r = utf8_encode(dst, c);
    return r == UTF8_INVALID ? -1 : (int)r;
}

int mblen(const char *s, size_t n) {
    return mbtowc((wchar_t *)0, s, n);
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n) {
    mbstate_t st = {0, 0, 0};
    size_t out = 0;
    const char *p = src;
    for (;;) {
        wchar_t wc = 0;
        size_t r = utf8_decode(&wc, p, 4, &st);
        if (r == UTF8_INVALID || r == UTF8_INCOMPLETE) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (wc == 0) {
            if (dst && out < n) {
                dst[out] = 0;
            }
            return out;
        }
        if (dst) {
            if (out >= n) {
                return out;
            }
            dst[out] = wc;
        }
        out++;
        p += r;
    }
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n) {
    char scratch[4];
    size_t out = 0;
    for (size_t i = 0; src[i]; i++) {
        size_t r = utf8_encode(scratch, src[i]);
        if (r == UTF8_INVALID) {
            return (size_t)-1;
        }
        if (dst) {
            if (out + r > n) {
                return out;
            }
            for (size_t k = 0; k < r; k++) {
                dst[out + k] = scratch[k];
            }
        }
        out += r;
    }
    if (dst && out < n) {
        dst[out] = 0;
    }
    return out;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *ps) {
    if (!src || !*src) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    mbstate_t *st = ps ? ps : &internal_state;
    const char *p = *src;
    size_t out = 0;
    for (;;) {
        if (dst && out >= n) {
            *src = p;
            return out;
        }
        wchar_t wc = 0;
        size_t r = utf8_decode(&wc, p, 4, st);
        if (r == UTF8_INVALID || r == UTF8_INCOMPLETE) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        p += r;
        if (wc == 0) {
            if (dst) {
                dst[out] = 0;
                *src = (const char *)0;
            }
            return out;
        }
        if (dst) {
            dst[out] = wc;
        }
        out++;
    }
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *ps) {
    (void)ps;
    if (!src || !*src) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    const wchar_t *p = *src;
    char scratch[4];
    size_t out = 0;
    for (;;) {
        if (*p == 0) {
            if (dst && out < n) {
                dst[out] = 0;
                *src = (const wchar_t *)0;
            }
            return out;
        }
        size_t r = utf8_encode(scratch, *p);
        if (r == UTF8_INVALID) {
            return (size_t)-1;
        }
        if (dst) {
            if (out + r > n) {
                *src = p;
                return out;
            }
            for (size_t k = 0; k < r; k++) {
                dst[out + k] = scratch[k];
            }
        }
        out += r;
        p++;
    }
}

size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
                  mbstate_t *ps) {
    if (!src || !*src) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    mbstate_t *st = ps ? ps : &internal_state;
    const char *p = *src;
    size_t consumed = 0;
    size_t out = 0;
    for (;;) {
        if (dst && out >= len) {
            *src = p;
            return out;
        }
        if (consumed >= nms) {
            *src = p;
            return out;
        }
        size_t avail = nms - consumed;
        if (avail > 4) {
            avail = 4;
        }
        wchar_t wc = 0;
        size_t r = utf8_decode(&wc, p, avail, st);
        if (r == UTF8_INCOMPLETE) {
            *src = p + avail;
            return out;
        }
        if (r == UTF8_INVALID) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        p += r;
        consumed += r;
        if (wc == 0) {
            if (dst) {
                dst[out] = 0;
            }
            *src = (const char *)0;
            return out;
        }
        if (dst) {
            dst[out] = wc;
        }
        out++;
    }
}

size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                  mbstate_t *ps) {
    (void)ps;
    if (!src || !*src) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    const wchar_t *p = *src;
    char scratch[4];
    size_t seen = 0;
    size_t out = 0;
    for (;;) {
        if (seen >= nwc) {
            *src = p;
            return out;
        }
        if (*p == 0) {
            if (dst) {
                if (out >= len) {
                    *src = p;
                    return out;
                }
                dst[out] = 0;
            }
            *src = (const wchar_t *)0;
            return out;
        }
        size_t r = utf8_encode(scratch, *p);
        if (r == UTF8_INVALID) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (dst) {
            if (out + r > len) {
                *src = p;
                return out;
            }
            for (size_t k = 0; k < r; k++) {
                dst[out + k] = scratch[k];
            }
        }
        out += r;
        p++;
        seen++;
    }
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    wchar_t discard = 0;
    return mbrtowc(&discard, s, n, ps ? ps : &internal_state);
}

wint_t btowc(int c) {
    if (c == EOF) {
        return WEOF;
    }
    unsigned char b = (unsigned char)c;
    return b < 0x80 ? (wint_t)b : WEOF;
}

int wctob(wint_t c) {
    if (c == WEOF) {
        return EOF;
    }
    return (c >= 0 && c < 0x80) ? (int)c : EOF;
}

size_t wcsftime(wchar_t *out, size_t n, const wchar_t *fmt,
                const struct tm *tm) {
    if (n == 0) {
        return 0;
    }
    size_t fmt_len = wcslen(fmt);
    char *nfmt = (char *)malloc(fmt_len * 4 + 1);
    char *nout = (char *)malloc(n * 4 + 1);
    size_t produced = 0;
    if (nfmt && nout) {
        const wchar_t *fp = fmt;
        mbstate_t st;
        for (size_t i = 0; i < sizeof(st); i++) {
            ((unsigned char *)&st)[i] = 0;
        }
        if (wcsrtombs(nfmt, &fp, fmt_len * 4 + 1, &st) != (size_t)-1) {
            size_t r = strftime(nout, n * 4 + 1, nfmt, tm);
            if (r > 0 || nout[0] == '\0') {
                const char *np = nout;
                mbstate_t st2;
                for (size_t i = 0; i < sizeof(st2); i++) {
                    ((unsigned char *)&st2)[i] = 0;
                }
                size_t w = mbsrtowcs(out, &np, n, &st2);
                if (w != (size_t)-1 && w < n) {
                    out[w] = 0;
                    produced = w;
                }
            }
        }
    }
    free(nfmt);
    free(nout);
    return produced;
}

static int wcs_is_delim(wchar_t c, const wchar_t *delim) {
    for (; *delim; delim++) {
        if (*delim == c) {
            return 1;
        }
    }
    return 0;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr) {
    if (!saveptr || !delim) {
        return 0;
    }
    wchar_t *p = s ? s : *saveptr;
    if (!p) {
        return 0;
    }
    while (*p && wcs_is_delim(*p, delim)) {
        p++;
    }
    if (!*p) {
        *saveptr = p;
        return 0;
    }
    wchar_t *start = p;
    while (*p && !wcs_is_delim(*p, delim)) {
        p++;
    }
    if (*p) {
        *p = 0;
        p++;
    }
    *saveptr = p;
    return start;
}

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WFMT_MAX 1024

static int wfmt_to_mb(const wchar_t *fmt, char *out, size_t cap) {
    size_t at = 0;
    for (size_t i = 0; fmt[i]; i++) {
        char mb[8];
        int n = (int)wcrtomb(mb, fmt[i], (mbstate_t *)0);
        if (n <= 0 || at + (size_t)n + 1 >= cap) {
            break;
        }
        for (int k = 0; k < n; k++) {
            out[at++] = mb[k];
        }
    }
    out[at] = '\0';
    return (int)at;
}

int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap) {
    char narrow[WFMT_MAX];
    if (!fmt) {
        return -1;
    }
    wfmt_to_mb(fmt, narrow, sizeof(narrow));
    return vfprintf(f, narrow, ap);
}

int fwprintf(FILE *f, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfwprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vwprintf(const wchar_t *fmt, va_list ap) {
    return vfwprintf(stdout, fmt, ap);
}

int wprintf(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vwprintf(fmt, ap);
    va_end(ap);
    return n;
}

int vswprintf(wchar_t *out, size_t n, const wchar_t *fmt, va_list ap) {
    char narrow_fmt[WFMT_MAX];
    static char narrow_out[4096];
    if (!out || n == 0 || !fmt) {
        return -1;
    }
    wfmt_to_mb(fmt, narrow_fmt, sizeof(narrow_fmt));
    int produced = vsnprintf(narrow_out, sizeof(narrow_out), narrow_fmt, ap);
    if (produced < 0) {
        return -1;
    }
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    const char *src = narrow_out;
    size_t written = mbsrtowcs(out, &src, n - 1, &st);
    if (written == (size_t)-1) {
        return -1;
    }
    out[written] = L'\0';
    return src ? -1 : (int)written;
}

int swprintf(wchar_t *out, size_t n, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

int fputwc(wchar_t c, FILE *f) {
    char mb[8];
    int n = (int)wcrtomb(mb, c, (mbstate_t *)0);
    if (n <= 0) {
        return WEOF;
    }
    for (int i = 0; i < n; i++) {
        if (fputc(mb[i], f) == EOF) {
            return WEOF;
        }
    }
    return (int)c;
}

int putwc(wchar_t c, FILE *f) {
    return fputwc(c, f);
}

int putwchar(wchar_t c) {
    return fputwc(c, stdout);
}

wint_t getwc(FILE *f) {
    return fgetwc(f);
}

wint_t getwchar(void) {
    return fgetwc(stdin);
}

wchar_t *fgetws(wchar_t *ws, int n, FILE *f) {
    if (!ws || n <= 0) {
        return (wchar_t *)0;
    }
    int i = 0;
    while (i < n - 1) {
        wint_t c = fgetwc(f);
        if (c == WEOF) {
            if (i == 0) {
                return (wchar_t *)0;
            }
            break;
        }
        ws[i++] = (wchar_t)c;
        if (c == L'\n') {
            break;
        }
    }
    ws[i] = 0;
    return ws;
}

int fwide(FILE *f, int mode) {
    (void)f;
    (void)mode;
    return 0;
}

int fputws(const wchar_t *ws, FILE *f) {
    if (!ws) {
        return -1;
    }
    for (size_t i = 0; ws[i]; i++) {
        if (fputwc(ws[i], f) == WEOF) {
            return -1;
        }
    }
    return 0;
}
