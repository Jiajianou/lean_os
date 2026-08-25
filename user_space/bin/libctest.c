/* user_space/bin/libctest.c
 *
 * M63's self-test for the libc subset, and for the SSE support
 * underneath it. Spawned and waited on from kernel_main the way M19's
 * memtest and M57's fonttest are, and for the same reason: the thing
 * being proved only exists in ring 3.
 *
 * It is deliberately written *as a program*, using the standard headers
 * and nothing from this project - the same rules third_party/ is built
 * under. If it needed a lean_os header to work it would not be testing
 * what it claims to.
 *
 * The assertions are on values a maths library either gets right or does
 * not: sin(pi/6) is exactly a half, log(e) is exactly one, and the
 * formatter's job is that %e prints what the number is. Tolerances are
 * stated (1e-10) rather than "close enough", because math.h claims an
 * accuracy and this is where that claim can fail.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

static void fail(const char *what) {
    printf("[libctest] FAIL: %s\n", what);
    failures++;
}

static void near(const char *what, double got, double want) {
    double d = got - want;
    if (d < 0) {
        d = -d;
    }
    /* Relative for large values, absolute for small - the usual rule,
     * and the one that keeps sin(pi) (which should be ~0) from being
     * judged by a relative error against zero. */
    double scale = want < 0 ? -want : want;
    double tol = scale > 1.0 ? scale * 1e-10 : 1e-10;
    if (d > tol) {
        printf("[libctest] FAIL: %s: got %.15g want %.15g\n", what, got, want);
        failures++;
    }
}

static void same(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("[libctest] FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
        failures++;
    }
}

int main(void) {
    /* ---- floating point exists at all ---- */
    volatile double a = 3.0, b = 7.0;
    if (a / b * b != 3.0 && fabs(a / b * b - 3.0) > 1e-15) {
        fail("double division does not round-trip");
    }

    /* ---- math.h ---- */
    near("sqrt(2)", sqrt(2.0), 1.4142135623730951);
    near("sin(pi/6)", sin(M_PI / 6.0), 0.5);
    near("cos(pi/3)", cos(M_PI / 3.0), 0.5);
    near("sin(0)", sin(0.0), 0.0);
    near("sin(1000)", sin(1000.0), 0.8268795405320025);   /* range reduction, not the series */
    near("cos(-40)", cos(-40.0), -0.6669380616522619);
    near("atan(1)", atan(1.0), M_PI / 4.0);
    near("atan(1e6)", atan(1000000.0), 1.5707953267948966);
    near("atan2(1,-1)", atan2(1.0, -1.0), 2.356194490192345);
    near("exp(1)", exp(1.0), M_E);
    near("exp(-20)", exp(-20.0), 2.061153622438558e-09);
    near("log(e)", log(M_E), 1.0);
    near("log(1e9)", log(1000000000.0), 20.72326583694641);
    near("pow(2,10)", pow(2.0, 10.0), 1024.0);
    near("pow(2,0.5)", pow(2.0, 0.5), 1.4142135623730951);
    near("fmod(7,3)", fmod(7.0, 3.0), 1.0);
    near("floor(-1.5)", floor(-1.5), -2.0);
    near("ceil(-1.5)", ceil(-1.5), -1.0);

    /* ---- stdio's formatter ---- */
    char buf[128];
    snprintf(buf, sizeof(buf), "%d %5d %-5d| %05d", 42, 42, 42, 42);
    same("integer widths", buf, "42    42 42   | 00042");
    snprintf(buf, sizeof(buf), "%x %X %o %u", 255u, 255u, 8u, 4000000000u);
    same("bases", buf, "ff FF 10 4000000000");
    snprintf(buf, sizeof(buf), "%s|%8s|%-8s|%.3s", "ab", "ab", "ab", "abcdef");
    same("string widths", buf, "ab|      ab|ab      |abc");
    snprintf(buf, sizeof(buf), "%.2f %.0f %.4f", 3.14159, 2.5, -0.00005);
    same("fixed point", buf, "3.14 3 -0.0001");
    snprintf(buf, sizeof(buf), "%e %.2e %12.4e", 1234.5, 0.000271828, 1234.5);
    same("scientific", buf, "1.234500e+03 2.72e-04   1.2345e+03");
    snprintf(buf, sizeof(buf), "%ld %c %%", 1234567890L, 'z');
    same("long and char", buf, "1234567890 z %");
    /* Truncation reports what it *would* have written, which is what
     * makes snprintf usable for sizing. */
    if (snprintf(buf, 4, "abcdef") != 6 || strcmp(buf, "abc") != 0) {
        fail("snprintf truncation");
    }

    /* ---- stdlib ---- */
    if (atoi("  -42xyz") != -42) {
        fail("atoi");
    }
    if (strtol("0x1f", (char **)0, 0) != 31) {
        fail("strtol base detection");
    }
    near("strtod", strtod("-3.5e2", (char **)0), -350.0);
    {
        char *p = (char *)malloc(100);
        memset(p, 0xAB, 100);
        char *q = (char *)realloc(p, 200);
        if (!q) {
            fail("realloc returned nothing");
        } else {
            for (int i = 0; i < 100; i++) {
                if ((unsigned char)q[i] != 0xAB) {
                    fail("realloc did not preserve the old contents");
                    break;
                }
            }
            free(q);
        }
        int *z = (int *)calloc(64, sizeof(int));
        for (int i = 0; i < 64; i++) {
            if (z[i] != 0) {
                fail("calloc did not zero");
                break;
            }
        }
        free(z);
    }

    /* ---- string ---- */
    {
        char s[32] = "hello";
        strcat(s, ", world");
        same("strcat", s, "hello, world");
        if (!strstr(s, "o, w") || strstr(s, "zzz")) {
            fail("strstr");
        }
        if (strchr(s, 'w') != s + 7 || strrchr(s, 'o') != s + 8) {
            fail("strchr/strrchr");
        }
        char ov[16] = "abcdefgh";
        memmove(ov + 2, ov, 6);
        same("memmove overlapping", ov, "ababcdef");
    }

    /* ---- time ---- */
    if (time((time_t *)0) <= 0) {
        fail("time() has no clock");
    }

    /* ---- and that a task switch preserves all of this ----
     *
     * The one thing a single-threaded test cannot check by arithmetic
     * alone: FXSAVE/FXRSTOR around a context switch. A long float loop
     * spans many scheduler quanta, so if the switch did not preserve xmm
     * state this sum would come back wrong - and it would come back
     * wrong *intermittently*, which is exactly the kind of bug worth a
     * deterministic assertion. */
    {
        double sum = 0.0;
        for (int i = 1; i <= 200000; i++) {
            sum += 1.0 / ((double)i * (double)i);
        }
        /* Converging on pi^2/6; 200000 terms gets within 5e-6 of it. */
        double want = M_PI * M_PI / 6.0;
        if (fabs(sum - want) > 1e-5) {
            printf("[libctest] FAIL: a float loop across scheduler quanta lost state: %.12f vs %.12f\n",
                   sum, want);
            failures++;
        }
    }

    if (failures) {
        return 1;
    }
    printf("[libctest] libc subset and SSE verified.\n");
    return 0;
}
