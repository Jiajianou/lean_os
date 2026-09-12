/* tests/clang/hello.c - M121's fixture.
 *
 * Compiled by x86_64-lean_os-clang with **no flag supplied by hand** -
 * the same standard tests/gcc/hello.c is held to and for the same
 * reason. See tools/clang-test.sh, which is the one place the compile
 * line is written and where you can check it is `$CC hello.c -o
 * clangtest` and nothing else.
 *
 * ---- why this is not a copy of tests/gcc/hello.c ----------------------
 *
 * The first five checks are that fixture's, deliberately: the two
 * compilers link against the SAME crt1.o, the same libc.a and the same
 * libgcc.a, so anything the first one proves is worth proving again
 * about the second. The rest are here because they are what a SECOND
 * compiler on this target can get wrong while the first one is right,
 * and each of them corresponds to a decision in
 * tools/clang-port/LeanOS.cpp:
 *
 *   - 128-bit division, which no x86-64 machine has an instruction for.
 *     clang emits a call to __divti3, which lives in GCC's libgcc.a -
 *     so this line is the evidence that the port reaches M94's runtime
 *     library rather than a compiler-rt that was deliberately not built.
 *   - the LOAD ADDRESS, checked from inside the program. A lean_os
 *     program lives at 512 GiB and the small code model's 32-bit
 *     relocations do not reach it; the failure of the code-model
 *     decision is a link error, but the failure of getting it right in
 *     the compiler and wrong in the linker script is a program that
 *     loads somewhere else and still runs. This asks where it is.
 *   - a SIGNAL delivered over live stack data, which is the red zone.
 *     The kernel builds the signal frame on the process's own stack
 *     (M76), so a program compiled with a red zone loses 128 bytes of
 *     whatever was under %rsp the first time it takes a signal - and
 *     loses it silently, which is why this is a checksum rather than a
 *     printf.
 *   - setjmp/longjmp, which is where a compiler that keeps a live value
 *     in a caller-saved register across setjmp() is found out.
 *   - a thread-local, which is the local-exec TLS model a static
 *     program here uses.
 *   - an atomic read-modify-write, which clang lowers inline and which
 *     the M79 threads and M96 futexes underneath this libc depend on.
 *
 * It prints one line per check, and the [m121] boot self-test greps for
 * the last one - which only appears if every earlier line did.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int constructed;
static int destructed_ok;

/* A global with a known value, so the load-address check below is
 * reading something the linker placed rather than a stack address. */
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

/* ---- the red zone, made observable ---------------------------------
 *
 * `scratch` is 256 bytes of pattern on the stack and the handler runs
 * while it is live. With -mno-red-zone the kernel's signal frame is
 * built below everything this function is using; without it, the frame
 * lands on top of the 128 bytes under %rsp - which on a frame this size
 * is inside `scratch`. So the checksum either survives or it does not,
 * and nothing else in a program reports this.
 *
 * volatile so the array cannot be kept in registers, and noinline so it
 * really has a frame of its own.
 */
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

    /* ---- libgcc, reached from clang's code generator ---------------- */
    volatile __int128 num = (__int128)1 << 100;
    volatile __int128 den = (__int128)3 << 40;
    __int128 quo = num / den;
    /* 2^100 / (3 * 2^40) = 2^60 / 3 = 384307168202282325 */
    if (quo != (__int128)384307168202282325LL) {
        printf("clangtest: FAIL 128-bit division (__divti3)\n");
        return 7;
    }
    printf("clangtest: 128-bit division through libgcc\n");

    /* ---- where this program actually is ---------------------------- */
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
