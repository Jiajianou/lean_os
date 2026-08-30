/* user_space/libc/src/wchar.c - M80 groundwork. See <wchar.h> for what
 * this is and, more importantly, for what it is deliberately wrong
 * about. */
#include <wchar.h>

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

/* ---- the conversions, which are Latin-1 and say so ------------------ */

size_t mbstowcs(wchar_t *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i]; i++) {
        if (dst && i < n) {
            dst[i] = (wchar_t)(unsigned char)src[i];
        }
        if (dst && i + 1 == n) {
            return n; /* no room for the terminator, same as everywhere */
        }
    }
    if (dst && i < n) {
        dst[i] = 0;
    }
    return i;
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n) {
    size_t i = 0;
    for (; src[i]; i++) {
        if (src[i] > 0xFF) {
            return (size_t)-1; /* not representable - see the header note */
        }
        if (dst && i < n) {
            dst[i] = (char)(unsigned char)src[i];
        }
        if (dst && i + 1 == n) {
            return n;
        }
    }
    if (dst && i < n) {
        dst[i] = 0;
    }
    return i;
}

int mbtowc(wchar_t *dst, const char *src, size_t n) {
    if (!src) {
        return 0; /* "is this encoding stateful" - no */
    }
    if (n == 0) {
        return -1;
    }
    if (dst) {
        *dst = (wchar_t)(unsigned char)*src;
    }
    return *src ? 1 : 0;
}

int wctomb(char *dst, wchar_t c) {
    if (!dst) {
        return 0;
    }
    if (c > 0xFF) {
        return -1;
    }
    *dst = (char)(unsigned char)c;
    return 1;
}

size_t mbrtowc(wchar_t *dst, const char *src, size_t n, mbstate_t *ps) {
    (void)ps; /* nothing to carry - Latin-1 is stateless */
    if (!src) {
        return 0;
    }
    if (n == 0) {
        return (size_t)-2; /* incomplete, which for a one-byte encoding means "you gave me nothing" */
    }
    if (dst) {
        *dst = (wchar_t)(unsigned char)*src;
    }
    return *src ? 1 : 0;
}

size_t wcrtomb(char *dst, wchar_t c, mbstate_t *ps) {
    (void)ps;
    if (!dst) {
        return 1;
    }
    if (c > 0xFF) {
        return (size_t)-1;
    }
    *dst = (char)(unsigned char)c;
    return 1;
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
