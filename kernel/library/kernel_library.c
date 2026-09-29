#include "kernel_library.h"

/* M197. These were byte loops, and a kernel built with -mgeneral-regs-only
   gets no vectorised copy from the compiler either: zeroing a page was 4,096
   stores, and exec'ing a 300 MB browser zeroed and copied 77,000 of them.
   Every x86-64 core this runs on has fast string moves (ERMSB on the
   laptop's Tiger Lake), which move a cache line a cycle with no vector
   registers. The host build keeps the loop, since the tests run on arm64.

   Both clear the direction flag first. Nothing on the way into the kernel
   does, and a program interrupted inside a backward copy arrives with it
   set: a string move would then run downwards from the address it was
   given, over whatever lies below it. A byte loop never noticed. */
void *k_memcpy(void *destination, const void *source, size_t n) {
#if defined(__x86_64__)
    void *d = destination;
    __asm__ volatile("cld; rep movsb" : "+D"(d), "+S"(source), "+c"(n) : : "memory");
    return destination;
#else
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return destination;
#endif
}

void *k_memmove(void *destination, const void *source, size_t n) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    if (d == s || n == 0) {
        return destination;
    }
    if (d > s && d < s + n) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    } else {
        return k_memcpy(destination, source, n);
    }
    return destination;
}

void *k_memset(void *destination, int c, size_t n) {
#if defined(__x86_64__)
    void *d = destination;
    __asm__ volatile("cld; rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return destination;
#else
    unsigned char *d = (unsigned char *)destination;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return destination;
#endif
}

size_t k_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int k_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

int k_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void k_strlcpy(char *destination, const char *source, size_t n) {
    if (n == 0) {
        return;
    }
    size_t i = 0;
    for (; i < n - 1 && source[i]; i++) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
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
