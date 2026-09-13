#include "string_utilities.h"

#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (n >= sizeof(uint64_t) && ((uintptr_t)d % sizeof(uint64_t)) == ((uintptr_t)s % sizeof(uint64_t))) {
        while (((uintptr_t)d % sizeof(uint64_t)) != 0) {
            *d++ = *s++;
            n--;
        }
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (n >= sizeof(uint64_t)) {
            *d64++ = *s64++;
            n -= sizeof(uint64_t);
        }
        d = (unsigned char *)d64;
        s = (const unsigned char *)s64;
    }

    while (n > 0) {
        *d++ = *s++;
        n--;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
