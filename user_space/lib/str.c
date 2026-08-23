#include "str.h"

#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    /* Copy 8 bytes at a time via plain GPR loads/stores - safe under
     * -mgeneral-regs-only (Makefile), unlike an SSE-vectorized copy this
     * freestanding kernel has never set up FPU/SSE state for - whenever
     * src and dst share the same 8-byte alignment; a misaligned pair, or
     * whatever's left below 8 bytes, still goes through the byte loop
     * below. Whole-framebuffer-row copies (user_space/bin/compositor.c's
     * present()) are the case this matters for: 8x fewer loop iterations
     * on a multi-megabyte copy that runs every redraw. */
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
