#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#include "malloc.h"
#include "syscall_wrappers.h"

/* calloc and realloc live in user_space/library/malloc.c with malloc and
   free, not here. An allocator has to be ONE archive member: a program that
   brings its own - Chromium's PartitionAlloc shim does - defines every name
   in the set, and a linker that pulls this object for some other symbol in
   it would then find two of each. M145 hit exactly that. */


extern void __lean_run_exit_handlers(void);
extern void __lean_stdio_flush_all(void);

void exit(int status) {
    __lean_run_exit_handlers();
    __lean_stdio_flush_all();
    sys_exit(status);
    for (;;) {
    }
}

void abort(void) {
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
            double factor = 1.0;
            for (int i = 0; i < exp10; i++) {
                factor *= 10.0;
            }
            value = eneg ? value / factor : value * factor;
        } else {
            p = save;
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

static unsigned long rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state / 65536u) % 32768u);
}

/* rand_r(3), which is rand with its state in the caller's hand rather than in
   this library's. That is the whole of the difference and it is the reason
   POSIX has it: two threads calling rand() share one sequence and one
   unsynchronised update of it. The generator is the same one rand() uses, so
   a caller that seeds both the same way gets the same numbers.

   libwebm asked (M160). */
int rand_r(unsigned int *state) {
    if (!state) {
        errno = EINVAL;
        return 0;
    }
    *state = *state * 1103515245u + 12345u;
    return (int)((*state / 65536u) % 32768u);
}

void srand(unsigned int seed) {
    rand_state = seed;
}

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

/* The six X's are checked ONCE and written many times, and that separation is
   the whole of this. These four calls all retry on a name that is taken, and
   until M159 the retry could not work: each attempt re-validated the template
   before filling it, and the attempt before had already replaced the X's with
   letters - so attempt 1 saw a template that was no longer one, returned
   EINVAL, and the loop gave up on its second time round. Nothing noticed for
   a hundred and fifty milestones because reaching attempt 1 needs a name
   collision, which needs the same process in the same millisecond with the
   file already there. mkstemps' own test produced exactly that.

   suffix_length is mkstemps(3)'s: the number of characters AFTER the X's that
   are part of the name rather than of the template. It returns the index one
   past the last X, or zero for a string that is not a template - which is
   never a valid answer, because six X's cannot end before position six. */
static size_t template_span(const char *template, size_t suffix_length) {
    size_t n = 0;
    while (template[n]) {
        n++;
    }
    if (n < suffix_length + 6) {
        return 0;
    }
    n -= suffix_length;
    for (size_t i = n - 6; i < n; i++) {
        if (template[i] != 'X') {
            return 0;
        }
    }
    return n;
}

static void fill_template_span(char *template, size_t end, unsigned int salt) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = end - 6; i < end; i++) {
        template[i] = alphabet[salt % (sizeof(alphabet) - 1)];
        salt /= (sizeof(alphabet) - 1);
        salt += 7919;
    }
}

static unsigned int template_salt(void) {
    return (unsigned int)sys_getpid() * 2654435761u +
           (unsigned int)sys_uptime_ms();
}

static int make_temporary_file(char *template, size_t suffix_length) {
    if (!template) {
        errno = EINVAL;
        return -1;
    }
    size_t end = template_span(template, suffix_length);
    if (end == 0) {
        errno = EINVAL;
        return -1;
    }
    unsigned int salt = template_salt();
    for (int attempt = 0; attempt < 128; attempt++) {
        fill_template_span(template, end,
                           salt + (unsigned int)attempt * 104729u);
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL);
        if (fd >= 0) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *template) {
    return make_temporary_file(template, 0);
}

/* mkstemps(3), which is mkstemp with a suffix the template keeps - so a
   caller that needs the name to end in ".png" can have one. It is 4.4BSD's
   and glibc carries it; ANGLE's system_utils_posix.cpp is the caller that
   asked here (M159). A negative suffix_length is the one thing it can be
   handed that means nothing. */
int mkstemps(char *template, int suffix_length) {
    if (suffix_length < 0) {
        errno = EINVAL;
        return -1;
    }
    return make_temporary_file(template, (size_t)suffix_length);
}

char *mktemp(char *template) {
    if (!template) {
        errno = EINVAL;
        return (char *)0;
    }
    size_t end = template_span(template, 0);
    if (end == 0) {
        errno = EINVAL;
        return (char *)0;
    }
    unsigned int salt = template_salt();
    for (int attempt = 0; attempt < 128; attempt++) {
        fill_template_span(template, end,
                           salt + (unsigned int)attempt * 104729u);
        if (access(template, F_OK) != 0) {
            return template;
        }
    }
    template[0] = '\0';
    errno = EEXIST;
    return template;
}

char *mkdtemp(char *template) {
    if (!template) {
        errno = EINVAL;
        return (char *)0;
    }
    size_t end = template_span(template, 0);
    if (end == 0) {
        errno = EINVAL;
        return (char *)0;
    }
    unsigned int salt = template_salt();
    for (int attempt = 0; attempt < 128; attempt++) {
        fill_template_span(template, end,
                           salt + (unsigned int)attempt * 104729u);
        if (mkdir(template, 0700) == 0) {
            return template;
        }
    }
    errno = EEXIST;
    return (char *)0;
}

static unsigned int random_state = 1;

long random(void) {
    random_state = random_state * 1103515245u + 12345u;
    return (long)(random_state >> 1);
}

void srandom(unsigned int seed) {
    random_state = seed;
}

char *initstate(unsigned int seed, char *state, size_t n) {
    (void)n;
    random_state = seed;
    return state;
}

char *setstate(char *state) {
    return state;
}

/* strtold is in user_space/libc/src/strtold.c. It used to be here, as
   (long double)strtod(s, end), which returned a double's answer to a
   question a double cannot hold - the trap M142 named and refused. */

float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

long long atoll(const char *s) {
    return strtoll(s, (char **)0, 10);
}

long long llabs(long long v) {
    return v < 0 ? -v : v;
}

div_t div(int num, int den) {
    div_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}

ldiv_t ldiv(long num, long den) {
    ldiv_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}

lldiv_t lldiv(long long num, long long den) {
    lldiv_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}
