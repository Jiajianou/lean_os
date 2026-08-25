#include "libk.h"

void *k_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *k_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

size_t k_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int k_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void k_strlcpy(char *dst, const char *src, size_t n) {
    if (n == 0) {
        return;
    }
    size_t i = 0;
    for (; i < n - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

const char *k_strstr(const char *haystack, const char *needle) {
    if (!needle[0]) {
        return haystack;
    }
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (needle[i] && h[i] == needle[i]) {
            i++;
        }
        if (!needle[i]) {
            return h;
        }
    }
    return (const char *)0;
}
