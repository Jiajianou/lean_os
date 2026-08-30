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
