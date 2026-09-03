#include <stdlib.h>
#include <errno.h>     /* M89: mkstemp/mkdtemp report why they gave up */
#include <fcntl.h>     /* M89: O_CREAT|O_EXCL, which is what makes mkstemp safe */
#include <unistd.h>    /* M98: access/F_OK, which is all mktemp can honestly do */
#include <string.h>
#include <sys/stat.h>  /* M89: mkdir, for mkdtemp */

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

/* M94: exit runs what was registered; _exit does not. See
 * __lean_run_exit_handlers in env.c, and <unistd.h>'s note on _exit,
 * which predicted this distinction becoming real. */
extern void __lean_run_exit_handlers(void);

void exit(int status) {
    __lean_run_exit_handlers();
    sys_exit(status);
    for (;;) {
    }
}

void abort(void) {
    /* Deliberately does NOT run the exit handlers. abort() means the
     * program has decided its own state is not trustworthy, and running
     * a flush over a corrupt buffer is how a crash turns into a
     * corrupted file. C says the same thing in more words. */
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

/* ---- M89: mkstemp/mkdtemp - see <stdlib.h> for the exclusion argument */

/* The candidate name is built from three things that differ between two
 * processes racing here: the pid, the uptime in milliseconds, and a
 * counter that advances on every attempt. None of them is a random
 * number, and this is not a security boundary - O_EXCL is what makes the
 * result correct, and these only decide how many attempts it takes. */
static int fill_template(char *template, unsigned int salt) {
    size_t n = 0;
    while (template[n]) {
        n++;
    }
    if (n < 6) {
        return -1;
    }
    for (size_t i = n - 6; i < n; i++) {
        if (template[i] != 'X') {
            return -1;
        }
    }
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = n - 6; i < n; i++) {
        template[i] = alphabet[salt % (sizeof(alphabet) - 1)];
        salt /= (sizeof(alphabet) - 1);
        salt += 7919; /* so the six characters do not all collapse to one */
    }
    return 0;
}

int mkstemp(char *template) {
    if (!template) {
        errno = EINVAL;
        return -1;
    }
    unsigned int salt =
        (unsigned int)sys_getpid() * 2654435761u + (unsigned int)sys_uptime_ms();
    for (int attempt = 0; attempt < 128; attempt++) {
        if (fill_template(template, salt + (unsigned int)attempt * 104729u) != 0) {
            errno = EINVAL;
            return -1;
        }
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL);
        if (fd >= 0) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

/* M98's probe found this missing, and it is the one of the three that
 * cannot be made safe.
 *
 * `mktemp` picks a name that does not exist and RETURNS it, so between
 * the check and whatever the caller does with it anything may take the
 * name. mkstemp exists precisely because of that gap, and every modern
 * system marks this deprecated. It is here anyway, and the reason is the
 * same one <sys/mount.h> gives about refusing rather than stubbing: GNU
 * libiberty's choose-temp.c calls it by name, so absence is a build
 * failure in the first file of the first library of binutils - and a
 * caller that has decided it wants this behaviour is not made safer by
 * being unable to link.
 *
 * What IS done about it: the name is chosen with the same three inputs
 * mkstemp uses and is verified not to exist, and both headers say what
 * the race is. */
char *mktemp(char *template) {
    if (!template) {
        errno = EINVAL;
        return (char *)0;
    }
    unsigned int salt =
        (unsigned int)sys_getpid() * 2654435761u + (unsigned int)sys_uptime_ms();
    for (int attempt = 0; attempt < 128; attempt++) {
        if (fill_template(template, salt + (unsigned int)attempt * 104729u) != 0) {
            errno = EINVAL;
            return (char *)0;
        }
        if (access(template, F_OK) != 0) {
            return template;
        }
    }
    /* POSIX: an empty string, not the untouched template - a caller that
     * used it anyway would otherwise get a name ending in XXXXXX. */
    template[0] = '\0';
    errno = EEXIST;
    return template;
}

char *mkdtemp(char *template) {
    if (!template) {
        errno = EINVAL;
        return (char *)0;
    }
    unsigned int salt =
        (unsigned int)sys_getpid() * 2654435761u + (unsigned int)sys_uptime_ms();
    for (int attempt = 0; attempt < 128; attempt++) {
        if (fill_template(template, salt + (unsigned int)attempt * 104729u) != 0) {
            errno = EINVAL;
            return (char *)0;
        }
        if (mkdir(template, 0700) == 0) {
            return template;
        }
    }
    errno = EEXIST;
    return (char *)0;
}

/* ---- M89: random()/srandom() ----------------------------------------
 *
 * The same linear congruential generator rand() uses, widened to
 * random()'s 31-bit range. BSD's random() is historically a trinomial
 * additive-feedback generator with a much longer period, and this is not
 * that - which is worth stating rather than implying, because the whole
 * reason a program calls random() instead of rand() is that it wants the
 * better one.
 *
 * What it is NOT is a source of unpredictability, and neither is BSD's:
 * both are deterministic from the seed. Nothing on this machine should
 * be using either for anything that needs to be unguessable, and nothing
 * does - `shuf` is what asked for it.
 */
static unsigned int random_state = 1;

long random(void) {
    random_state = random_state * 1103515245u + 12345u;
    return (long)(random_state >> 1); /* 31 bits, which is random()'s range */
}

void srandom(unsigned int seed) {
    random_state = seed;
}

char *initstate(unsigned int seed, char *state, size_t n) {
    /* The state array is not used: this generator's whole state is one
     * 32-bit word (see above), so there is nothing to spread across the
     * caller's buffer. Accepting the array and ignoring it is right -
     * the caller's contract is that it owns the storage, not that the
     * library must use all of it - and returning it back is what
     * setstate() would be handed. */
    (void)n;
    random_state = seed;
    return state;
}

char *setstate(char *state) {
    return state; /* see initstate: there is one state and it is not here */
}

/* ---- M89 - see <stdlib.h> and <string.h> for what each of these is -- */

long double strtold(const char *s, char **end) {
    return (long double)strtod(s, end);
}

float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

long long atoll(const char *s) {
    return strtoll(s, (char **)0, 10);
}

long long llabs(long long v) {
    return v < 0 ? -v : v;
}

/* M97: see <stdlib.h> for why these three exist at all. Written as one
 * division each so the compiler emits the single idiv that computes both
 * halves, which is the only argument these functions ever had. */
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

/* M97: declared, and it refuses. See <stdlib.h> for the argument - the
 * short version is that a system() nothing calls is a system() nothing
 * checks, and this project has refused to ship those since M65. */
int system(const char *command) {
    if (!command) {
        return 0; /* "is there a command processor" - no, not through this */
    }
    return -1;
}
