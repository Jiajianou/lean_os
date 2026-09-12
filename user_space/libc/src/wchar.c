/* user_space/libc/src/wchar.c - M80 groundwork. See <wchar.h> for what
 * this is and, more importantly, for what it is deliberately wrong
 * about. */
#include <wchar.h>

#include <errno.h> /* M88: EILSEQ - a byte sequence this encoding refuses */
#include <stdlib.h> /* M121: wcsdup allocates, like strdup does */
#include <limits.h> /* M121: LONG_MAX/ULONG_MAX - the saturation below */
#include <time.h>   /* M121: wcsftime narrows into strftime - see below */

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

/* ---- M121: the searches libc++ found missing --------------------------
 *
 * See the note in <wchar.h>. Each is the narrow function of the same
 * name with wchar_t in place of char, which is what the standard says
 * they are - there is nothing encoding-dependent in any of them,
 * because a wchar_t here is a code point and comparing two code points
 * is comparing two integers.
 */
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
    /* An empty needle matches at the start, which is what strstr does
     * and what a caller looping over matches depends on. */
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

/* Collation in the C locale, which is code-point order - see the note in
 * <wchar.h> and the identical one in <string.h>. */
int wcscoll(const wchar_t *a, const wchar_t *b) {
    return wcscmp(a, b);
}

/* And the transform whose only requirement is that comparing two
 * transformed strings with wcscmp gives the same answer as wcscoll on
 * the originals. In this locale that makes the transform a copy, and the
 * return value is the length the result needed - which, like strxfrm's,
 * may exceed `n`. */
size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n) {
    size_t len = wcslen(src);
    if (n > 0) {
        size_t copy = len < n - 1 ? len : n - 1;
        wmemcpy(dst, src, copy);
        dst[copy] = 0;
    }
    return len;
}

/* ---- the numeric conversions ------------------------------------------
 *
 * Written here rather than by narrowing into a buffer and calling
 * strtol: a buffer would need a length, and the one thing this cannot
 * do is refuse a long number. The digit walk is a dozen lines and has no
 * failure mode. */
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
    /* ---- M121: "0x" with no hex digit after it ---------------------
     *
     * C defines the subject sequence as the LONGEST initial subsequence
     * of the expected form, and `0x` on its own is not of that form -
     * `0` is. So the value is zero and the end pointer belongs on the
     * `x`, not back at the start of the string. This consumed the two
     * characters and then reported "no digits at all", which put the end
     * pointer at `s` and told a caller parsing a list that nothing was
     * there. Found by tests/test_wcs.c against the host's strtoll, and
     * `after_zero` below is where the answer is kept. */
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
    /* ---- M121: overflow, which this stopped short of --------------
     *
     * M80 accumulated without checking, so `wcstol` on a value past
     * LONG_MAX wrapped and returned a plausible wrong number. C
     * requires saturation at the limit with ERANGE, and the difference
     * matters here more than the word "overflow" suggests: a wrapped
     * result is a *smaller* number that a bounds check accepts.
     *
     * Found by tools/../tests/test_wcs.c comparing against the host's
     * `strtoll` - `9223372036854775807` in base 36 came back as
     * 3707029189907929095 - and by UBSan on the negation below, which
     * is the same bug at the other end: -(long)v where v is 2^63 is
     * undefined, and the value it is undefined about is LONG_MIN,
     * which is the one input a sign-handling path must get right.
     *
     * The check is done before the multiply rather than after, because
     * after is where the information has already been lost. */
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
        /* The scan continues past an overflow on purpose: C requires the
         * end pointer to point past the WHOLE subject sequence whatever
         * the value did, so a caller parsing a list stays in step. */
    }
    if (end) {
        if (p != digits) {
            *end = (wchar_t *)p;
        } else if (after_zero) {
            *end = (wchar_t *)after_zero; /* "0x" - see the note above */
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
    /* LONG_MIN is representable and -LONG_MIN is not, so the comparison
     * is against the magnitude as an UNSIGNED value and the negation is
     * done in unsigned arithmetic. That is the whole of what UBSan
     * objected to, and the reason it objected only for one input. */
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
    /* A negative value is not an error for strtoul: it is negated in
     * unsigned arithmetic, so `-1` is ULONG_MAX. That is what C says and
     * what every other implementation does. */
    return neg ? (0UL - v) : v;
}

/* ---- M121: the long long pair ----------------------------------------
 *
 * Widened rather than re-parsed - see the note in <wchar.h>. This target
 * is LP64 and always will be (x86-64 only since M26), so `long` already
 * holds everything `long long` does and the conversion loses nothing.
 * The day a 32-bit target appears is the day wcs_to_ul needs a wider
 * accumulator, and the assertion below is what will notice.
 */
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

/* ---- M121: and the floating-point three ------------------------------
 *
 * Narrowed into strtod rather than parsed here, and the reason it is
 * safe to narrow is worth stating: every character that can appear
 * *inside* a numeric literal is ASCII - whitespace, a sign, digits,
 * `.`, `e`/`E`/`p`/`P`, `0x`, and the letters of `inf` and `nan`. So the
 * copy below stops at the first character that cannot be part of one,
 * and up to that point one wide character is exactly one byte. That is
 * what makes the end pointer correct: the byte offset strtod reports is
 * the wide-character offset, with no conversion.
 *
 * A second floating-point parser would be the alternative, and this
 * project already has one that tools/scanf-test.sh and
 * tools/printf-test.sh grade against the host's. One parser.
 */
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

/* Copies the leading numeric-looking run into `buf` and returns how many
 * characters were taken. */
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

/* ---- M111: mbsinit, and why its absence broke a build far from here ---
 *
 * Three lines, and it was missing for twenty-three milestones with
 * nothing noticing - because nothing in this tree calls it. GNU grep
 * does not call it either. What it does is *probe* for it, and gnulib's
 * rule when the probe fails is not "do without": it is
 *
 *     no mbsinit  =>  this platform's mbstate_t cannot be trusted
 *                 =>  typedef int rpl_mbstate_t; #define mbstate_t rpl_mbstate_t
 *
 * and then it substitutes its own mbrtowc to go with it - but NOT
 * wcrtomb, because this libc has a wcrtomb and gnulib saw no reason to
 * replace it. So dfa.c ended up passing a `rpl_mbstate_t *` (an `int *`)
 * to a `wcrtomb` declared here as taking this file's real 8-byte
 * mbstate_t, and the build stopped on a type error in a function nobody
 * here wrote, about a state object nobody here asked for.
 *
 * That is the second time M94's lesson has arrived in this exact shape:
 * a missing symbol is not a missing feature, it is a *configure answer*,
 * and the substitution it triggers lands somewhere with no relation to
 * the thing that was absent. Worth the three lines.
 *
 * UTF-8 is stateless between characters, so "is this the initial state"
 * is exactly "is there a partial sequence in flight". A NULL `ps` is the
 * initial state by definition, which is what the standard says and what
 * every caller that passes NULL is relying on. */
int mbsinit(const mbstate_t *ps) {
    return !ps || ps->owed == 0;
}

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

/* ---- M121: the n-limited restartable forms ----------------------------
 *
 * POSIX's `mbsnrtowcs` and `wcsnrtombs`: the two above with a limit on
 * the INPUT as well as the output. That second limit is the whole reason
 * they exist - a program converting a buffer that is not NUL-terminated
 * (a mapped file, a network frame, a std::string_view) cannot use the
 * `mbsrtowcs` form at all, because that one reads until it finds a zero
 * and there may not be one. libc++'s locale layer is such a program.
 *
 * Both stop early and report how far they got, which is what makes them
 * restartable: `*src` is left pointing at the first unconsumed input,
 * or set to NULL when the terminator was consumed. An input window that
 * ends mid-character is NOT an error - it is the case these are for, and
 * the partial sequence stays in `*ps` for the next call.
 */
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
            /* The window ended inside a character. Not an error: the
             * state holds what was read and the next call finishes it. */
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

/* `mbrtowc` with nowhere to put the character, which is exactly how it
 * is written rather than a second decoder. */
size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    wchar_t discard = 0;
    return mbrtowc(&discard, s, n, ps ? ps : &internal_state);
}

/* ---- M121: the single-byte special cases -----------------------------
 *
 * `btowc` asks "is this byte a complete character on its own, and which
 * one" and `wctob` asks the reverse. On a UTF-8 system the answer is
 * "only if it is ASCII", and saying so is the whole implementation -
 * which is worth a note because it is a place a libc can quietly be
 * wrong: returning the byte's value for 0x80..0xFF would be the Latin-1
 * answer this project deliberately stopped giving in M88.
 */
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

/* ---- M121: wcsftime, narrowed into strftime --------------------------
 *
 * Built on the narrow formatter rather than beside it, which is the same
 * decision M94 made for the wide printf family and for the same reason
 * (see the note in <wchar.h> about wprintf): a second implementation of
 * the conversion table is a second thing to keep correct, and every
 * conversion strftime supports is ASCII.
 *
 * The two buffers are allocated rather than fixed because both bounds
 * come from the caller: a format of any length, and an output measured
 * in wide characters that can need up to four bytes each. Out of memory
 * is reported as 0 with nothing written, which is what wcsftime's
 * "did not fit" answer already is.
 */
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

/* ---- M94: the wide stdio family --------------------------------------
 *
 * Built on the narrow one rather than beside it, and the reason is worth
 * a paragraph because "a second formatter for wide strings" is the
 * obvious design and is the wrong one.
 *
 * A wide printf's format is a wchar_t string and its conversions are the
 * same conversions. So: the format is converted to multibyte once, and
 * handed to vsnprintf - which M94 taught `%ls` and `%lc`, because those
 * are what a NARROW printf does with wide arguments anyway. One
 * formatter, one set of bugs, and `wprintf(L"%ls\n", s)` and
 * `printf("%ls\n", s)` cannot disagree because they are the same code.
 *
 * The output is bytes, in this locale's encoding, which is UTF-8 (M88).
 * That is what a wide printf is specified to produce on a stream: the
 * wideness is in the arguments, not on the wire.
 *
 * What is NOT here: the stream orientation rules (fwide, and the rule
 * that a stream used once wide may not then be used narrow). This stdio
 * has no buffering and no per-stream state to orient - see stdio.c - so
 * there is nothing for an orientation to mean, and a program that mixes
 * the two gets exactly what it asked for in the order it asked.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The longest wide format this converts. A format string is written by
 * the programmer rather than by input, so this is a bound on source
 * text; anything past it is truncated, which shows up immediately as a
 * missing tail rather than as a wrong value. */
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

/* swprintf writes WIDE characters into a wide buffer, so unlike the
 * stream forms it has to convert back. `n` is a count of wide
 * characters including the NUL, which is the one place swprintf differs
 * from snprintf in more than spelling - snprintf's is bytes. */
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
    /* -1 when it did not all fit, which is what C specifies for
     * swprintf and is NOT what snprintf does (that returns what it would
     * have written). The difference is real and is the reason a caller
     * cannot size a buffer by calling swprintf with n == 0. */
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

/* ---- M121: the wide input family's four wrappers --------------------
 *
 * fgetwc and ungetwc are in stdio.c, where struct FILE is visible; these
 * four need nothing from it. See the note there.
 */
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
            /* Nothing read at all is a failure; a partial line followed
             * by end of input is a line. That asymmetry is what lets a
             * caller loop on the return value and still see a file whose
             * last line has no newline. */
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

/* ---- fwide, and why it always answers "no orientation" --------------
 *
 * C says a stream becomes byte- or wide-oriented on its first use and
 * cannot change afterwards, and that fwide reports or sets that. **This
 * libc does not track it**, and this function says so rather than
 * inventing a value: 0 means "no orientation", which is the truthful
 * answer for a stream that has never been given one and the answer a
 * caller must already handle.
 *
 * The reason it is not tracked is the same reason `mbstate_t` is small:
 * an orientation exists so that an implementation with two different
 * buffers can refuse to mix them, and there is one buffer here. fputwc
 * encodes to bytes and writes them through the same path fputc uses, so
 * mixing wide and narrow output on one stream produces exactly what the
 * program asked for rather than undefined behaviour. A `mode` argument
 * is accepted and ignored, which is what a stream with no orientation to
 * set means.
 */
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
