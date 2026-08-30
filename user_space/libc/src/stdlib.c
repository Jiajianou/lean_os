#include <stdlib.h>
#include <string.h>

#include "malloc.h"           /* user_space/lib - malloc/free, M19 */
#include "syscall_wrappers.h" /* sys_exit */

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    /* Overflow is the one thing calloc is *for* checking - a caller that
     * multiplied itself would have allocated a small buffer and written
     * a large one. */
    if (count != 0 && total / count != size) {
        return (void *)0;
    }
    void *p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

/* No size header to consult, so this cannot know the old block's length -
 * see malloc.c, whose free list keeps that privately. Copying `size`
 * bytes is safe when growing and correct when shrinking; the case it
 * cannot serve is growing *from* a block smaller than the new size,
 * where it would read past the old allocation. malloc.c's header is
 * right there, so this asks it. */
void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return (void *)0;
    }
    size_t old = malloc_usable_size(ptr);
    void *p = malloc(size);
    if (!p) {
        return (void *)0;
    }
    memcpy(p, ptr, old < size ? old : size);
    free(ptr);
    return p;
}

void exit(int status) {
    sys_exit(status);
    for (;;) {
    }
}

void abort(void) {
    /* Nonzero and distinctive: a process that aborted did not finish, and
     * a task manager row showing 134 (128 + SIGABRT, the convention
     * everywhere) says which kind of not-finishing it was. */
    sys_exit(134);
    for (;;) {
    }
}

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

long strtol(const char *s, char **end, int base) {
    const char *p = s;
    while (is_space(*p)) {
        p++;
    }
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }
    long value = 0;
    int any = 0;
    for (;;) {
        int d = digit_value((unsigned char)*p);
        if (d < 0 || d >= base) {
            break;
        }
        value = value * base + d;
        any = 1;
        p++;
    }
    if (end) {
        *end = (char *)(any ? p : s);
    }
    return neg ? -value : value;
}

double strtod(const char *s, char **end) {
    const char *p = s;
    while (is_space(*p)) {
        p++;
    }
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    double value = 0.0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10.0 + (double)(*p - '0');
        any = 1;
        p++;
    }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            value += (double)(*p - '0') * scale;
            scale *= 0.1;
            any = 1;
            p++;
        }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *save = p;
        p++;
        int eneg = 0;
        if (*p == '+' || *p == '-') {
            eneg = (*p == '-');
            p++;
        }
        if (*p >= '0' && *p <= '9') {
            int exp10 = 0;
            while (*p >= '0' && *p <= '9') {
                exp10 = exp10 * 10 + (*p - '0');
                p++;
            }
            /* Repeated multiplication rather than a pow() call: this is
             * the one place in the library that must not depend on
             * math.c, which is allowed to depend on this one. */
            double factor = 1.0;
            for (int i = 0; i < exp10; i++) {
                factor *= 10.0;
            }
            value = eneg ? value / factor : value * factor;
        } else {
            p = save; /* "1e" is a number followed by a letter, not a bad number */
        }
    }
    if (end) {
        *end = (char *)(any ? p : s);
    }
    return neg ? -value : value;
}

int atoi(const char *s) {
    return (int)strtol(s, (char **)0, 10);
}

long atol(const char *s) {
    return strtol(s, (char **)0, 10);
}

double atof(const char *s) {
    return strtod(s, (char **)0);
}

int abs(int v) {
    return v < 0 ? -v : v;
}

long labs(long v) {
    return v < 0 ? -v : v;
}

/* The exact generator C89 prints in its own specification - chosen for
 * that reason rather than for its quality, so a ported program that was
 * written against "whatever rand() does" gets the behaviour its author
 * most likely saw. */
static unsigned long rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state / 65536u) % 32768u);
}

void srand(unsigned int seed) {
    rand_state = seed;
}

/* ---- M80 groundwork ---------------------------------------------------
 *
 * The unsigned and long-long conversions, and the two search/sort
 * routines every C program eventually reaches for. Each shares the
 * digit-parsing shape strtol already had rather than reimplementing it,
 * because two parsers that disagree about what "0x" means is exactly the
 * kind of near-duplicate this project keeps out.
 */
static unsigned long long strtoull_common(const char *s, char **end, int base, int *negated) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') {
        p++;
    }
    *negated = 0;
    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        *negated = 1;
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long long acc = 0;
    const char *digits_start = p;
    for (; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') {
            d = *p - '0';
        } else if (*p >= 'a' && *p <= 'z') {
            d = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'Z') {
            d = *p - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        acc = acc * (unsigned long long)base + (unsigned long long)d;
    }
    if (end) {
        /* No digits at all means nothing was consumed, which the standard
         * signals by handing back the ORIGINAL string rather than the
         * point the sign or prefix reached. */
        *end = (char *)(p == digits_start ? s : p);
    }
    return acc;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtoull_common(s, end, base, &neg);
    return neg ? (unsigned long)(-(unsigned long)v) : (unsigned long)v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtoull_common(s, end, base, &neg);
    return neg ? (unsigned long long)(-v) : v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtoull_common(s, end, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

/* Insertion sort over a byte-wise swap. Not quicksort, and the reason is
 * this project's own measure-first rule: the callers here sort tens of
 * elements, where an insertion sort is genuinely faster than a
 * partitioning one and is a third of the code. The day something sorts
 * ten thousand elements and says so, this is the function to replace -
 * and its interface will not change when it is.
 *
 * Stable, which qsort is not required to be and which costs nothing
 * here; a caller that depended on it would be depending on an accident,
 * so this is a note rather than a promise. */
void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *a = (unsigned char *)base;
    for (size_t i = 1; i < count; i++) {
        for (size_t j = i; j > 0; j--) {
            unsigned char *lo = a + (j - 1) * size;
            unsigned char *hi = a + j * size;
            if (cmp(lo, hi) <= 0) {
                break;
            }
            for (size_t b = 0; b < size; b++) {
                unsigned char t = lo[b];
                lo[b] = hi[b];
                hi[b] = t;
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
               int (*cmp)(const void *, const void *)) {
    const unsigned char *a = (const unsigned char *)base;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, a + mid * size);
        if (r == 0) {
            return (void *)(a + mid * size);
        }
        if (r < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return (void *)0;
}
