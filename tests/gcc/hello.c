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

    char *buffer = malloc(256);
    if (!buffer) {
        printf("gcctest: FAIL malloc\n");
        return 2;
    }
    strcpy(buffer, "allocated");
    if (strcmp(buffer, "allocated") != 0) {
        printf("gcctest: FAIL string round trip\n");
        return 3;
    }
    free(buffer);
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
