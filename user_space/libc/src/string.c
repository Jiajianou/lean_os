/* Only what user_space/lib/str.c does not already provide - see
 * string.h's header comment on why these are not two sets. */
#include <string.h>

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
