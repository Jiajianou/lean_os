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
