/* Only what user_space/lib/str.c does not already provide - see
 * string.h's header comment on why these are not two sets. */
#include <string.h>
#include <stdlib.h> /* malloc - strdup, M80 groundwork */

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    /* Overlap is the whole reason this is not memcpy: copying forward
     * through a region that overlaps ahead of itself reads bytes it has
     * already written. */
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return (void *)0;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) {
            return (int)x - (int)y;
        }
        if (x == 0) {
            return 0;
        }
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*dst++ = *src++)) {
    }
    return out;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    /* The standard's own strange rule: pad with NULs to exactly n, and
     * do *not* terminate if src was longer. Kept faithful rather than
     * improved, because third-party code is written against the strange
     * rule. */
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *out = dst;
    while (*dst) {
        dst++;
    }
    while ((*dst++ = *src++)) {
    }
    return out;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *out = dst;
    while (*dst) {
        dst++;
    }
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return out;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (*s == '\0') {
            return (char *)0;
        }
    }
}

char *strrchr(const char *s, int c) {
    const char *found = (const char *)0;
    for (;; s++) {
        if (*s == (char)c) {
            found = s;
        }
        if (*s == '\0') {
            return (char *)found;
        }
    }
}

char *strstr(const char *haystack, const char *needle) {
    if (!needle[0]) {
        return (char *)haystack;
    }
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (needle[i] && h[i] == needle[i]) {
            i++;
        }
        if (!needle[i]) {
            return (char *)h;
        }
    }
    return (char *)0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; s[n]; n++) {
        if (!strchr(accept, s[n])) {
            break;
        }
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; n++) {
        if (strchr(reject, s[n])) {
            break;
        }
    }
    return n;
}

/* ---- M80 groundwork: strerror -----------------------------------------
 *
 * The E* *name*, not a sentence. See <errno.h> for why: this kernel
 * returns -1 without a reason for almost everything, so a libc that
 * printed "No such file or directory" would be describing a failure
 * nothing actually reported. A name is exactly as much as is known.
 *
 * A code with no entry comes back as "error", which is also true.
 */
char *strerror(int errnum) {
    static const struct {
        int code;
        const char *name;
    } NAMES[] = {
        {1, "EPERM"},   {2, "ENOENT"},  {3, "ESRCH"},   {4, "EINTR"},
        {5, "EIO"},     {9, "EBADF"},   {10, "ECHILD"}, {11, "EAGAIN"},
        {12, "ENOMEM"}, {13, "EACCES"}, {14, "EFAULT"}, {16, "EBUSY"},
        {17, "EEXIST"}, {20, "ENOTDIR"},{21, "EISDIR"}, {22, "EINVAL"},
        {23, "ENFILE"}, {24, "EMFILE"}, {28, "ENOSPC"}, {29, "ESPIPE"},
        {32, "EPIPE"},  {33, "EDOM"},   {34, "ERANGE"}, {36, "ENAMETOOLONG"},
        {38, "ENOSYS"}, {39, "ENOTEMPTY"},
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (NAMES[i].code == errnum) {
            return (char *)NAMES[i].name;
        }
    }
    return (char *)"error";
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) {
                return (char *)s;
            }
        }
    }
    return (char *)0;
}

/* Allocates, which is why it lives here rather than in str.c with the
 * copies that do not: a program that calls strdup has already accepted
 * that it owns the result and must free it. */
char *strdup(const char *s) {
    if (!s) {
        return (char *)0;
    }
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out) {
        return (char *)0;
    }
    memcpy(out, s, n);
    return out;
}

/* ---- M89 - see <string.h> for what each of these buys ---------------- */

char *strndup(const char *s, size_t n) {
    if (!s) {
        return (char *)0;
    }
    size_t len = 0;
    while (len < n && s[len]) {
        len++;
    }
    char *p = (char *)malloc(len + 1);
    if (!p) {
        return (char *)0;
    }
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src) != '\0') {
        dst++;
        src++;
    }
    return dst; /* the NUL, not the start - that is the whole point */
}

char *stpncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    char *end = dst + i;
    /* strncpy's zero-fill, which stpncpy inherits: the remainder is
     * padded, and the return is the first NUL written rather than the
     * end of the padding. */
    while (i < n) {
        dst[i++] = '\0';
    }
    return end;
}

void *memmem(const void *haystack, size_t hlen, const void *needle,
             size_t nlen) {
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    if (nlen == 0) {
        return (void *)h; /* the empty needle is at the start - POSIX's rule */
    }
    if (nlen > hlen) {
        return (void *)0;
    }
    /* The naive scan. A source tree's worth of `wget` output is a few
     * hundred kilobytes against a needle of a dozen bytes, so the O(hn)
     * worst case is not reachable by anything this machine does - and a
     * Boyer-Moore table would be more code than the thing it speeds up.
     * Said here so the next reader knows it was a choice. */
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nlen) == 0) {
            return (void *)(h + i);
        }
    }
    return (void *)0;
}

void *memccpy(void *dst, const void *src, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    unsigned char stop = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
        if (s[i] == stop) {
            return d + i + 1;
        }
    }
    return (void *)0;
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) {
        i++;
    }
    return i;
}

/* ---- M97: strtok, and the two locale functions that are not ----------
 *
 * strtok holds its position between calls in a static, which is exactly
 * the design C89 chose and exactly why it cannot be used from two
 * threads. strtok_r is the same walk with the caller holding the state,
 * and strtok is written in terms of it so there is one tokeniser here
 * rather than two that could disagree about a run of delimiters.
 */
char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (!s) {
        s = *saveptr;
    }
    if (!s) {
        return (char *)0;
    }
    /* Skip leading delimiters. A run of them is ONE separator, which is
     * the behaviour that distinguishes strtok from a field splitter and
     * the thing a hand-written version usually gets wrong. */
    while (*s && strchr(delim, *s)) {
        s++;
    }
    if (!*s) {
        *saveptr = (char *)0;
        return (char *)0;
    }
    char *token = s;
    while (*s && !strchr(delim, *s)) {
        s++;
    }
    if (*s) {
        *s = '\0';
        *saveptr = s + 1;
    } else {
        *saveptr = (char *)0;
    }
    return token;
}

char *strtok(char *s, const char *delim) {
    static char *saved;
    return strtok_r(s, delim, &saved);
}

/* This machine has one locale, and in the "C" locale the standard says
 * strcoll IS strcmp and strxfrm IS a copy. So these are not stubs and
 * not approximations - they are the specified behaviour for the only
 * locale that exists here, which is the same argument M65 made about
 * chmod: a machine with one principal reports one principal.
 *
 * strxfrm returns the length it WOULD have written, and copies only if
 * there is room. Getting that backwards is the classic bug: a caller
 * sizes a buffer from the return value of a first call with n == 0, and
 * a version that returned the copied length would tell it zero. */
int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

size_t strxfrm(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n > 0) {
        size_t copy = len < n - 1 ? len : n - 1;
        for (size_t i = 0; i < copy; i++) {
            dst[i] = src[i];
        }
        dst[copy] = '\0';
    }
    return len;
}
