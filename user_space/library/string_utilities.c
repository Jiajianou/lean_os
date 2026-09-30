#include "string_utilities.h"

#include <stdint.h>

/* M200. memcpy, memset and strlen were byte loops, or near enough - memcpy
   went a word at a time only when both pointers happened to share an
   alignment - and every program on this machine links them, Chromium
   included: PartitionAlloc's zeroing, every mojo message, V8's heap clears
   and Skia's bitmap clears all came here one byte per iteration.

   Copies and fills now move sixteen bytes at a time through unaligned vector
   loads and stores, and a short one is two overlapping stores rather than a
   loop. The searches and compares look at eight bytes a word. It is written
   with the compiler's vector types rather than SSE intrinsics so that the
   same source compiles for the host the differential test runs on
   (tools/string-test.sh), which is arm64. None of it is rep movsb: under
   the emulator the budgets are graded on, a string instruction is a loop of
   single steps, and a speed-up only real hardware sees would be paid for in
   every boot the harness times. */

typedef unsigned char vector16_t __attribute__((vector_size(16), aligned(1), may_alias));
typedef uint64_t __attribute__((may_alias, aligned(1))) unaligned_u64_t;
typedef uint32_t __attribute__((may_alias, aligned(1))) unaligned_u32_t;

#define ONES  0x0101010101010101ULL
#define HIGHS 0x8080808080808080ULL

static inline vector16_t load16(const unsigned char *p) {
    return *(const vector16_t *)p;
}

static inline void store16(unsigned char *p, vector16_t v) {
    *(vector16_t *)p = v;
}

static inline uint64_t load8(const unsigned char *p) {
    return *(const unaligned_u64_t *)p;
}

/* Every load happens before any store, so this is safe for a memmove of up
   to sixteen bytes in either direction as well. */
static inline void copy_up_to_16(unsigned char *d, const unsigned char *s, size_t n) {
    if (n >= 8) {
        uint64_t head = load8(s);
        uint64_t tail = load8(s + n - 8);
        *(unaligned_u64_t *)d = head;
        *(unaligned_u64_t *)(d + n - 8) = tail;
    } else if (n >= 4) {
        uint32_t head = *(const unaligned_u32_t *)s;
        uint32_t tail = *(const unaligned_u32_t *)(s + n - 4);
        *(unaligned_u32_t *)d = head;
        *(unaligned_u32_t *)(d + n - 4) = tail;
    } else if (n > 0) {
        unsigned char first = s[0];
        unsigned char middle = s[n / 2];
        unsigned char last = s[n - 1];
        d[0] = first;
        d[n / 2] = middle;
        d[n - 1] = last;
    }
}

/* Forward, sixteen bytes at a time, with the last sixteen loaded before the
   loop and stored after it. Safe for an overlapping move whose destination is
   BELOW its source: each chunk is read before any store that could reach it. */
static inline __attribute__((always_inline)) void copy_forward(unsigned char *d, const unsigned char *s, size_t n) {
    vector16_t tail = load16(s + n - 16);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        vector16_t a = load16(s + i);
        vector16_t b = load16(s + i + 16);
        vector16_t c = load16(s + i + 32);
        vector16_t e = load16(s + i + 48);
        store16(d + i, a);
        store16(d + i + 16, b);
        store16(d + i + 32, c);
        store16(d + i + 48, e);
    }
    for (; i + 16 <= n; i += 16) {
        store16(d + i, load16(s + i));
    }
    store16(d + n - 16, tail);
}

static inline __attribute__((always_inline)) void copy_backward(unsigned char *d, const unsigned char *s, size_t n) {
    vector16_t head = load16(s);
    size_t i = n;
    for (; i >= 64 + 16; i -= 64) {
        vector16_t a = load16(s + i - 16);
        vector16_t b = load16(s + i - 32);
        vector16_t c = load16(s + i - 48);
        vector16_t e = load16(s + i - 64);
        store16(d + i - 16, a);
        store16(d + i - 32, b);
        store16(d + i - 48, c);
        store16(d + i - 64, e);
    }
    for (; i >= 16 + 16; i -= 16) {
        store16(d + i - 16, load16(s + i - 16));
    }
    if (i > 16) {
        store16(d + i - 16, load16(s + i - 16));
    }
    store16(d, head);
}

void *memcpy(void *destination, const void *source, size_t n) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    if (n <= 16) {
        copy_up_to_16(d, s, n);
    } else {
        copy_forward(d, s, n);
    }
    return destination;
}

void *memmove(void *destination, const void *source, size_t n) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    if (d == s || n == 0) {
        return destination;
    }
    if (n <= 16) {
        copy_up_to_16(d, s, n);
    } else if ((uintptr_t)d - (uintptr_t)s >= n) {
        copy_forward(d, s, n);
    } else {
        copy_backward(d, s, n);
    }
    return destination;
}

void *memset(void *destination, int c, size_t n) {
    unsigned char *d = (unsigned char *)destination;
    unsigned char value = (unsigned char)c;
    uint64_t word = ONES * value;
    if (n < 16) {
        if (n >= 8) {
            *(unaligned_u64_t *)d = word;
            *(unaligned_u64_t *)(d + n - 8) = word;
        } else if (n >= 4) {
            *(unaligned_u32_t *)d = (uint32_t)word;
            *(unaligned_u32_t *)(d + n - 4) = (uint32_t)word;
        } else if (n > 0) {
            d[0] = value;
            d[n / 2] = value;
            d[n - 1] = value;
        }
        return destination;
    }
    vector16_t fill = {value, value, value, value, value, value, value, value,
                       value, value, value, value, value, value, value, value};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        store16(d + i, fill);
        store16(d + i + 16, fill);
        store16(d + i + 32, fill);
        store16(d + i + 48, fill);
    }
    for (; i + 16 <= n; i += 16) {
        store16(d + i, fill);
    }
    store16(d + n - 16, fill);
    return destination;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i = 0;
    while (i + 8 <= n && load8(x + i) == load8(y + i)) {
        i += 8;
    }
    for (; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

/* A byte of a word is zero exactly where this has its high bit set, and the
   lowest such bit is exact - the borrows that make the test imprecise only
   ever run towards higher bytes. Little-endian, so lowest is first. */
static inline uint64_t zero_bytes(uint64_t word) {
    return (word - ONES) & ~word & HIGHS;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    uint64_t pattern = ONES * (unsigned char)c;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t found = zero_bytes(load8(p + i) ^ pattern);
        if (found) {
            return (void *)(p + i + (size_t)(__builtin_ctzll(found) >> 3));
        }
    }
    for (; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return (void *)0;
}

/* Aligned words only: reading the whole aligned eight bytes a terminator is
   in never touches a page the string does not already touch, so this cannot
   fault where the byte loop would not have. */
size_t strlen(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (((uintptr_t)p & 7u) != 0) {
        if (*p == 0) {
            return (size_t)(p - (const unsigned char *)s);
        }
        p++;
    }
    for (;;) {
        uint64_t found = zero_bytes(*(const uint64_t *)p);
        if (found) {
            return (size_t)(p - (const unsigned char *)s) + (size_t)(__builtin_ctzll(found) >> 3);
        }
        p += 8;
    }
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
