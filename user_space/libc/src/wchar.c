/* user_space/libc/src/wchar.c - M80 groundwork. See <wchar.h> for what
 * this is and, more importantly, for what it is deliberately wrong
 * about. */
#include <wchar.h>

#include <errno.h> /* M88: EILSEQ - a byte sequence this encoding refuses */

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

/* ---- the numeric conversions ------------------------------------------
 *
 * Written here rather than by narrowing into a buffer and calling
 * strtol: a buffer would need a length, and the one thing this cannot
 * do is refuse a long number. The digit walk is a dozen lines and has no
 * failure mode. */
static unsigned long wcs_to_ul(const wchar_t *s, wchar_t **end, int base, int *neg) {
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
    if ((base == 0 || base == 16) && p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == L'0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long acc = 0;
    const wchar_t *digits = p;
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
        acc = acc * (unsigned long)base + (unsigned long)d;
    }
    if (end) {
        *end = (wchar_t *)(p == digits ? s : p);
    }
    return acc;
}

long wcstol(const wchar_t *s, wchar_t **end, int base) {
    int neg = 0;
    unsigned long v = wcs_to_ul(s, end, base, &neg);
    return neg ? -(long)v : (long)v;
}

unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base) {
    int neg = 0;
    unsigned long v = wcs_to_ul(s, end, base, &neg);
    return neg ? (unsigned long)(-(long)v) : v;
}

/* ---- the conversions, which are UTF-8 -------------------------------
 *
 * One decoder and one encoder, and every public function below is a
 * loop around one of them. That is deliberate: the interesting part of
 * UTF-8 is the set of sequences it must REFUSE - overlong forms, lone
 * continuation bytes, surrogates, anything above U+10FFFF - and a second
 * copy of those rules is a second place for one of them to be missing.
 * A decoder that accepts an overlong 0xC0 0xAF as '/' is how a path
 * check gets walked past, which is why it is rejected here rather than
 * left to the caller.
 */

#define UTF8_INCOMPLETE ((size_t)-2)
#define UTF8_INVALID    ((size_t)-1)

/* The smallest code point a sequence of this many continuation bytes is
 * allowed to encode. Anything below it is an overlong form: the same
 * character written the long way, which is a second spelling of a byte
 * string that has to have exactly one. */
static unsigned int utf8_min_for(unsigned char total) {
    switch (total) {
    case 1:  return 0x80u;
    case 2:  return 0x800u;
    default: return 0x10000u;
    }
}

static int utf8_is_valid(unsigned int wc, unsigned char total) {
    if (wc < utf8_min_for(total)) {
        return 0; /* overlong */
    }
    if (wc >= 0xD800u && wc <= 0xDFFFu) {
        return 0; /* a surrogate half is not a character - UTF-8 never encodes one */
    }
    return wc <= 0x10FFFFu;
}

/* The one decoder. Consumes bytes from `src` into `st`, and on a
 * complete character stores it through `out`. Returns the number of
 * bytes taken from THIS call (which is what mbrtowc reports), or
 * UTF8_INCOMPLETE if it ran out, or UTF8_INVALID with errno set. */
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
                /* 0x80-0xBF is a continuation with nothing to continue;
                 * 0xC0 and 0xC1 can only ever begin an overlong form, so
                 * they are refused here rather than three bytes later. */
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
                errno = EILSEQ; /* 0xF5 and up encode past U+10FFFF */
                return UTF8_INVALID;
            }
            continue;
        }
        if ((b & 0xC0u) != 0x80u) {
            /* A lead byte where a continuation was owed. The state is
             * cleared so the caller can carry on from the next
             * character rather than being stuck reporting EILSEQ
             * forever, which is what a decoder that kept the broken
             * state would do. */
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

/* The one encoder. Writes up to 4 bytes and returns how many, or
 * UTF8_INVALID with errno set for a code point UTF-8 cannot represent. */
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

/* The library's own state, for the calls whose `ps` may be NULL. Not
 * thread-safe and the standard says so; a program converting from two
 * threads at once passes its own. */
static mbstate_t internal_state;

size_t mbrtowc(wchar_t *dst, const char *src, size_t n, mbstate_t *ps) {
    mbstate_t *st = ps ? ps : &internal_state;
    if (!src) {
        /* mbrtowc(NULL, ...) means "reset the state and tell me whether
         * this encoding is stateful". UTF-8 is not stateful between
         * characters, so the answer is 0 - but any half-finished
         * sequence is abandoned, which is what the reset is for. */
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
    /* A completed L'\0' reports 0 bytes, not 1, which is how a caller
     * walking a string knows it has reached the end. */
    return wc == 0 ? 0 : r;
}

size_t wcrtomb(char *dst, wchar_t c, mbstate_t *ps) {
    (void)ps; /* nothing to carry: an encode is complete in one call */
    char scratch[4];
    if (!dst) {
        /* "how many bytes to return to the initial state, plus L'\0'" -
         * one, and the character is ignored. */
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
        return 0; /* "is this encoding stateful" - between characters, no */
    }
    mbstate_t st = {0, 0, 0};
    /* Decoded into a local and copied out, rather than straight through
     * `dst`. The return value has to be 0 for a NUL character whether or
     * not the caller wanted the character itself, and a version that
     * tested `*dst` could not answer that when dst is NULL - which is
     * exactly how mblen() calls this. Found by the test that says mblen
     * agrees with the decoder; the first version reported 1. */
    wchar_t wc = 0;
    size_t r = utf8_decode(&wc, src, n, &st);
    if (r == UTF8_INVALID || r == UTF8_INCOMPLETE) {
        /* mbtowc has no way to say "incomplete" separately, which is
         * the whole reason mbrtowc exists. Both are -1 here. */
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
        /* 4 is MB_CUR_MAX: the longest sequence UTF-8 has. The source is
         * NUL-terminated, so handing the decoder four bytes can read at
         * most as far as the terminator - which ends the string and is
         * itself a complete character. */
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
                return out; /* "n wide characters written, no terminator" */
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
                /* A character that does not fit whole is not written at
                 * all. Writing its first byte and stopping would leave a
                 * truncated sequence, which is the one thing a UTF-8
                 * string must never contain. */
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
            *src = p; /* where the next call resumes - the point of this form */
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
                *src = (const char *)0; /* the terminator was consumed */
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
