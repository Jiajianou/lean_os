#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int constructed;
static int destructed_ok;

static volatile long anchor = 0x5EA0L;

__attribute__((constructor))
static void ran_before_main(void) {
    constructed = 1;
}

static void ran_after_main(void) {
    if (destructed_ok) {
        printf("clangtest: atexit ran\n");
    }
}

struct point {
    long x;
    long y;
};

static struct point make_point(long x, long y) {
    struct point p;
    p.x = x;
    p.y = y;
    return p;
}

static int sum_varargs(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += va_arg(ap, int);
    }
    va_end(ap);
    return total;
}

static volatile int caught;

static void handler(int sig) {
    (void)sig;
    caught = 1;
}

__attribute__((noinline))
static int signal_over_live_stack(void) {
    volatile unsigned char scratch[256];
    unsigned long before = 0;
    for (int i = 0; i < 256; i++) {
        scratch[i] = (unsigned char)(i * 7 + 13);
        before += scratch[i];
    }
    if (signal(SIGUSR1, handler) == SIG_ERR) {
        return 0;
    }
    raise(SIGUSR1);
    unsigned long after = 0;
    for (int i = 0; i < 256; i++) {
        after += scratch[i];
    }
    return caught && before == after;
}

static jmp_buf jb;

__attribute__((noinline))
static void jump_back(int v) {
    longjmp(jb, v);
}

int main(void) {
    if (!constructed) {
        printf("clangtest: FAIL the constructor did not run\n");
        return 1;
    }
    printf("clangtest: constructor ran\n");

    char *buf = malloc(256);
    if (!buf) {
        printf("clangtest: FAIL malloc\n");
        return 2;
    }
    strcpy(buf, "allocated");
    if (strcmp(buf, "allocated") != 0) {
        printf("clangtest: FAIL string round trip\n");
        return 3;
    }
    free(buf);
    printf("clangtest: malloc and string round trip\n");

    struct point p = make_point(3, 4);
    if (p.x != 3 || p.y != 4) {
        printf("clangtest: FAIL struct return\n");
        return 4;
    }
    if (sum_varargs(4, 1, 2, 3, 4) != 10) {
        printf("clangtest: FAIL varargs\n");
        return 5;
    }
    printf("clangtest: struct return and varargs\n");

    volatile double a = 355.0;
    volatile double b = 113.0;
    double pi = a / b;
    if (pi < 3.1415 || pi > 3.1416) {
        printf("clangtest: FAIL floating point\n");
        return 6;
    }
    printf("clangtest: floating point\n");

    volatile __int128 num = (__int128)1 << 100;
    volatile __int128 den = (__int128)3 << 40;
    __int128 quo = num / den;
    if (quo != (__int128)384307168202282325LL) {
        printf("clangtest: FAIL 128-bit division (__divti3)\n");
        return 7;
    }
    printf("clangtest: 128-bit division through libgcc\n");

    if (anchor != 0x5EA0L) {
        printf("clangtest: FAIL a global did not survive the link\n");
        return 8;
    }
    uintptr_t where = (uintptr_t)(void *)&anchor;
    if (where < 0x8000000000ULL) {
        printf("clangtest: FAIL loaded at %lx, below this OS's image base\n",
               (unsigned long)where);
        return 9;
    }
    printf("clangtest: the large code model, at %lx\n", (unsigned long)where);

    if (!signal_over_live_stack()) {
        printf("clangtest: FAIL a signal corrupted live stack data "
               "(the red zone)\n");
        return 10;
    }
    printf("clangtest: a signal over live stack data\n");

    int jumped = setjmp(jb);
    if (jumped == 0) {
        jump_back(42);
        printf("clangtest: FAIL longjmp returned\n");
        return 11;
    }
    if (jumped != 42) {
        printf("clangtest: FAIL longjmp carried %d\n", jumped);
        return 12;
    }
    printf("clangtest: setjmp and longjmp\n");

    static __thread int tls_counter;
    tls_counter += 7;
    if (tls_counter != 7) {
        printf("clangtest: FAIL a thread-local did not hold its value\n");
        return 13;
    }
    printf("clangtest: a thread-local in a static program\n");

    static int atomic_slot;
    int prev = __atomic_fetch_add(&atomic_slot, 5, __ATOMIC_SEQ_CST);
    int now = __atomic_load_n(&atomic_slot, __ATOMIC_SEQ_CST);
    if (prev != 0 || now != 5) {
        printf("clangtest: FAIL an atomic read-modify-write\n");
        return 14;
    }
    printf("clangtest: an atomic read-modify-write\n");

    destructed_ok = 1;
    atexit(ran_after_main);
    printf("clangtest: every check passed\n");
    return 0;
}
