/* tests/gcc/hello.c - M94's fixture.
 *
 * Compiled by x86_64-lean_os-gcc with **no flag supplied by hand** -
 * which is the milestone, because every flag invented by hand is a flag
 * someone else's build system will not pass. See tools/gcc-test.sh,
 * which is the one place the compile line is written and where you can
 * check that it is `$CC hello.c -o gcctest` and nothing else.
 *
 * What it exercises, and why each line is here rather than a printf:
 *
 *   - a CONSTRUCTOR, which proves crti.o/crtbegin.o/crtend.o/crtn.o were
 *     linked in the right order and that .init_array is walked. A
 *     constructor that silently does not run is the failure mode a
 *     startup-file mistake actually has, and it is invisible to a
 *     program that only prints.
 *   - atexit, which proves the other end of the same machinery.
 *   - malloc and free, which reach this project's own allocator through
 *     libc.a out of the sysroot rather than through a link line.
 *   - a float, which pulls in libgcc (the compiler's own runtime, built
 *     for this target by the port) and SSE state (M63).
 *   - a struct returned by value and a varargs call, which are the two
 *     places a hand-written ABI usually diverges.
 *
 * It prints one line per check, and the boot self-test greps for the
 * last one - which only appears if every earlier line did.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int constructed;
static int destructed_ok;

__attribute__((constructor))
static void ran_before_main(void) {
    constructed = 1;
}

static void ran_after_main(void) {
    /* Nothing can observe this from inside the program, so it writes to
     * stdout - which the kernel's self-test reads. An atexit handler
     * that did not run would simply be a line that is not there. */
    if (destructed_ok) {
        printf("gcctest: atexit ran\n");
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

int main(void) {
    if (!constructed) {
        printf("gcctest: FAIL the constructor did not run\n");
        return 1;
    }
    printf("gcctest: constructor ran\n");

    char *buf = malloc(256);
    if (!buf) {
        printf("gcctest: FAIL malloc\n");
        return 2;
    }
    strcpy(buf, "allocated");
    if (strcmp(buf, "allocated") != 0) {
        printf("gcctest: FAIL string round trip\n");
        return 3;
    }
    free(buf);
    printf("gcctest: malloc and string round trip\n");

    struct point p = make_point(3, 4);
    if (p.x != 3 || p.y != 4) {
        printf("gcctest: FAIL struct return\n");
        return 4;
    }
    if (sum_varargs(4, 1, 2, 3, 4) != 10) {
        printf("gcctest: FAIL varargs\n");
        return 5;
    }
    printf("gcctest: struct return and varargs\n");

    /* A division the compiler cannot fold, so it is really SSE at run
     * time rather than a constant in .rodata. */
    volatile double a = 355.0;
    volatile double b = 113.0;
    double pi = a / b;
    if (pi < 3.1415 || pi > 3.1416) {
        printf("gcctest: FAIL floating point\n");
        return 6;
    }
    printf("gcctest: floating point\n");

    destructed_ok = 1;
    atexit(ran_after_main);
    printf("gcctest: every check passed\n");
    return 0;
}
