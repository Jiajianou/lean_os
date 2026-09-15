#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "math_long_double_table.h"

static int failures;
static int checks;

typedef long double (*one_argument)(long double);
typedef long double (*two_arguments)(long double, long double);
typedef long long (*integer_result)(long double);

static long long call_lroundl(long double x) { return (long long)lroundl(x); }
static long long call_llroundl(long double x) { return llroundl(x); }
static long long call_ilogbl(long double x) { return (long long)ilogbl(x); }

/* The generated table names functions; it cannot point at them, because an
   object that referenced this libm would be one the fold check could no
   longer tell apart from one that had failed to fold. So the binding lives
   here, by hand, and a name in the table with no row below is a failure. */
struct implementation {
    const char *name;
    one_argument one;
    two_arguments two;
    integer_result integer;
};

static const struct implementation IMPLEMENTATIONS[] = {
    {"fabsl", fabsl, 0, 0},
    {"copysignl", 0, copysignl, 0},
    {"sqrtl", sqrtl, 0, 0},
    {"truncl", truncl, 0, 0},
    {"floorl", floorl, 0, 0},
    {"ceill", ceill, 0, 0},
    {"roundl", roundl, 0, 0},
    {"lroundl", 0, 0, call_lroundl},
    {"llroundl", 0, 0, call_llroundl},
    {"ilogbl", 0, 0, call_ilogbl},
    {"logbl", logbl, 0, 0},
    {"expl", expl, 0, 0},
    {"exp2l", exp2l, 0, 0},
    {"expm1l", expm1l, 0, 0},
    {"logl", logl, 0, 0},
    {"log2l", log2l, 0, 0},
    {"log10l", log10l, 0, 0},
    {"log1pl", log1pl, 0, 0},
    {"sinl", sinl, 0, 0},
    {"cosl", cosl, 0, 0},
    {"tanl", tanl, 0, 0},
    {"asinl", asinl, 0, 0},
    {"acosl", acosl, 0, 0},
    {"atanl", atanl, 0, 0},
    {"atan2l", 0, atan2l, 0},
    {"sinhl", sinhl, 0, 0},
    {"coshl", coshl, 0, 0},
    {"tanhl", tanhl, 0, 0},
    {"asinhl", asinhl, 0, 0},
    {"acoshl", acoshl, 0, 0},
    {"atanhl", atanhl, 0, 0},
    {"hypotl", 0, hypotl, 0},
    {"cbrtl", cbrtl, 0, 0},
    {"powl", 0, powl, 0},
    {"fmodl", 0, fmodl, 0},
    {"remainderl", 0, remainderl, 0},
    {"fdiml", 0, fdiml, 0},
    {"fmaxl", 0, fmaxl, 0},
    {"fminl", 0, fminl, 0},
};

static const struct implementation *implementation_for(const char *name) {
    for (unsigned i = 0;
         i < sizeof(IMPLEMENTATIONS) / sizeof(IMPLEMENTATIONS[0]); i++) {
        if (strcmp(IMPLEMENTATIONS[i].name, name) == 0) {
            return &IMPLEMENTATIONS[i];
        }
    }
    return 0;
}

/* An 80-bit value's unit in the last place is 2^(exponent - 63). Taking it
   from the expected value rather than from ours means a wrong answer cannot
   widen its own tolerance. */
static long double units_in_last_place(long double ours, long double expected) {
    if (isnan(expected) || isnan(ours)) {
        return (isnan(expected) && isnan(ours)) ? 0.0L : (long double)INFINITY;
    }
    if (ours == expected) {
        return 0.0L;
    }
    if (isinf(expected) || isinf(ours)) {
        return (long double)INFINITY;
    }
    if (expected == 0.0L) {
        return (long double)INFINITY;
    }
    long double step = ldexpl(1.0L, ilogbl(expected) - 63);
    return fabsl(ours - expected) / step;
}

static void report(const char *name, long double worst, long double claimed,
                   int misses, unsigned count) {
    long long hundredths = (long long)(worst * 100.0L + 0.5L);
    if (isinf(worst)) {
        hundredths = -1;
    }
    if (misses == 0) {
        printf("mathltest: %-11s %4u points, worst %lld.%02lld ulp, claim %lld.%02lld\n",
               name, count, hundredths / 100, hundredths % 100,
               (long long)(claimed * 100.0L) / 100,
               (long long)(claimed * 100.0L) % 100);
    } else {
        printf("mathltest: FAIL %-11s %d of %u points past the claim, "
               "worst %lld.%02lld ulp\n",
               name, misses, count, hundredths / 100, hundredths % 100);
        failures++;
    }
}

static void grade_table(void) {
    for (unsigned f = 0; f < LONG_DOUBLE_FUNCTION_COUNT; f++) {
        const struct long_double_function *fn = &LONG_DOUBLE_FUNCTIONS[f];
        const struct implementation *impl = implementation_for(fn->name);
        if (impl == 0) {
            printf("mathltest: FAIL %s is in the table with nothing bound to "
                   "it\n", fn->name);
            failures++;
            continue;
        }
        long double worst = 0.0L;
        int misses = 0;
        for (unsigned i = 0; i < fn->count; i++) {
            const struct long_double_case *c = &fn->cases[i];
            checks++;
            if (fn->integer_result) {
                if (impl->integer(c->first) != c->expected_integer) {
                    misses++;
                    worst = (long double)INFINITY;
                }
                continue;
            }
            long double got = fn->two_arguments
                ? impl->two(c->first, c->second)
                : impl->one(c->first);
            long double distance = units_in_last_place(got, c->expected);
            if (distance > worst) {
                worst = distance;
            }
            if (distance > fn->max_ulp) {
                misses++;
            }
        }
        report(fn->name, worst, fn->max_ulp, misses, fn->count);
    }
}

static void check(const char *what, int ok) {
    checks++;
    if (!ok) {
        printf("mathltest: FAIL %s\n", what);
        failures++;
    }
}

/* The four functions GCC will not fold are the four that read the rounding
   mode, so the table has no answer for them. Round-to-nearest-even is not an
   approximation of anything, though - it is a definition, and a definition
   can be checked without an oracle. */
static void grade_rounding_mode(void) {
    int integral = 1, near = 1, ties_even = 1, agrees = 1, ties_seen = 0;
    for (int i = -20000; i <= 20000; i++) {
        long double x = (long double)i / 8.0L;
        long double r = rintl(x);
        if (r != truncl(r)) {
            integral = 0;
        }
        if (fabsl(r - x) > 0.5L) {
            near = 0;
        }
        if (fabsl(r - x) == 0.5L) {
            ties_seen++;
            if (fmodl(r, 2.0L) != 0.0L) {
                ties_even = 0;
            }
        }
        if (nearbyintl(x) != r || (long double)lrintl(x) != r ||
            (long double)llrintl(x) != r) {
            agrees = 0;
        }
    }
    check("rintl returns an integral value", integral);
    check("rintl is within half a unit of its argument", near);
    check("rintl breaks a tie to even", ties_even && ties_seen > 1000);
    check("nearbyintl, lrintl and llrintl agree with rintl", agrees);
}

static void grade_identities(void) {
    int ldexp_exact = 1, frexp_exact = 1, modf_exact = 1;
    for (int i = 1; i < 4000; i++) {
        long double x = (long double)i * 1.2345678901234567e-3L;
        for (int n = -60; n <= 60; n += 15) {
            long double scaled = ldexpl(x, n);
            long double back = scaled;
            for (int k = 0; k < (n < 0 ? -n : n); k++) {
                back = n < 0 ? back * 2.0L : back / 2.0L;
            }
            if (back != x) {
                ldexp_exact = 0;
            }
            if (scalbnl(x, n) != scaled || scalblnl(x, (long)n) != scaled) {
                ldexp_exact = 0;
            }
        }
        int exponent = 0;
        long double fraction = frexpl(x, &exponent);
        if (fabsl(fraction) < 0.5L || fabsl(fraction) >= 1.0L ||
            ldexpl(fraction, exponent) != x) {
            frexp_exact = 0;
        }
        long double integral = 0.0L;
        long double rest = modfl(x, &integral);
        if (integral + rest != x || integral != truncl(integral) ||
            fabsl(rest) >= 1.0L) {
            modf_exact = 0;
        }
    }
    check("ldexpl, scalbnl and scalblnl scale by a power of two exactly",
          ldexp_exact);
    check("frexpl splits into a fraction in [0.5, 1) and an exponent",
          frexp_exact);
    check("modfl splits into an integral part and a remainder", modf_exact);

    int remquo_ok = 1;
    for (int a = -800; a <= 800; a += 7) {
        for (int b = -40; b <= 40; b += 3) {
            if (b == 0) {
                continue;
            }
            long double x = (long double)a * 0.37L;
            long double y = (long double)b * 1.11L;
            int quotient = 0;
            long double r = remquol(x, y, &quotient);
            int bad = 0;
            if (r != remainderl(x, y)) {
                bad = 1;
            }
            /* C99 7.12.10.3: the quotient carries the sign of x/y and a
               magnitude congruent to |x/y| modulo 2^n for some n >= 3. The
               exact quotient is (x - r) / y and nothing else - taking
               rintl(x / y) instead asks a rounded division to agree with an
               exact remainder, which it does not have to. */
            long double exact = (x - r) / y;
            if (fabsl(exact - rintl(exact)) > 0.25L) {
                bad = 1;
            }
            long long whole = (long long)rintl(exact);
            long long magnitude = whole < 0 ? -whole : whole;
            int got = quotient < 0 ? -quotient : quotient;
            if ((long long)(got & 7) != (magnitude & 7)) {
                bad = 1;
            }
            if (quotient != 0 && (whole < 0) != (quotient < 0)) {
                bad = 1;
            }
            if (bad && remquo_ok) {
                printf("mathltest: remquol(%d * 0.37, %d * 1.11) gave quotient "
                       "%d where the exact quotient's low bits are %lld\n",
                       a, b, quotient, magnitude & 7);
            }
            if (bad) {
                remquo_ok = 0;
            }
        }
    }
    check("remquol agrees with remainderl and reports three quotient bits",
          remquo_ok);

    int step_ok = 1;
    long double samples[] = {
        1.0L, 2.0L, 0.5L, 1e-30L, 1e30L, 0x1p-16382L, 3.0L, 1024.0L,
    };
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        long double x = samples[i];
        long double up = nextafterl(x, (long double)INFINITY);
        long double down = nextafterl(x, -(long double)INFINITY);
        if (up <= x || down >= x) {
            step_ok = 0;
        }
        if (nextafterl(up, -(long double)INFINITY) != x ||
            nextafterl(down, (long double)INFINITY) != x) {
            step_ok = 0;
        }
        if (nexttowardl(x, (long double)INFINITY) != up) {
            step_ok = 0;
        }
        /* The step out of a binade's bottom is the one this format makes
           different: the integer bit is explicit, so it is a borrow. */
        if (up - x <= 0.0L || x - down <= 0.0L) {
            step_ok = 0;
        }
    }
    check("nextafterl and nexttowardl step one representable value and back",
          step_ok);
    check("nanl is a quiet NaN", isnan(nanl("")) != 0);
}

/* C99 F.9 decides these, not a measurement: they are the standard's own
   table, and a library that gets the sweep right and these wrong is wrong
   where somebody's error path lives. */
static void grade_special_values(void) {
    long double inf = (long double)INFINITY;
    long double nan = (long double)NAN;

    check("powl(x, 0) is 1 for every x", powl(nan, 0.0L) == 1.0L &&
          powl(inf, 0.0L) == 1.0L && powl(0.0L, 0.0L) == 1.0L &&
          powl(-3.0L, 0.0L) == 1.0L);
    check("powl(1, y) is 1 for every y", powl(1.0L, nan) == 1.0L &&
          powl(1.0L, inf) == 1.0L);
    check("powl(-2, 3) keeps the sign an odd integer exponent gives",
          powl(-2.0L, 3.0L) == -8.0L && powl(-2.0L, 2.0L) == 4.0L);
    check("powl of a negative base and a fractional exponent is NaN",
          isnan(powl(-2.0L, 0.5L)) != 0);
    check("powl(0, -1) is infinite", powl(0.0L, -1.0L) == inf);
    check("powl(-0, -3) is negative infinity",
          powl(-0.0L, -3.0L) == -inf);
    check("powl(2, inf) is infinite and powl(2, -inf) is zero",
          powl(2.0L, inf) == inf && powl(2.0L, -inf) == 0.0L);
    check("powl(0.5, inf) is zero and powl(0.5, -inf) is infinite",
          powl(0.5L, inf) == 0.0L && powl(0.5L, -inf) == inf);

    check("logl(0) is negative infinity and logl(-1) is NaN",
          logl(0.0L) == -inf && isnan(logl(-1.0L)) != 0);
    check("log1pl(-1) is negative infinity",
          log1pl(-1.0L) == -inf && isnan(log1pl(-2.0L)) != 0);
    check("sqrtl(-1) is NaN and sqrtl(inf) is infinite",
          isnan(sqrtl(-1.0L)) != 0 && sqrtl(inf) == inf);
    check("expl saturates rather than wrapping",
          expl(20000.0L) == inf && expl(-20000.0L) == 0.0L);
    check("exp2l saturates rather than wrapping",
          exp2l(20000.0L) == inf && exp2l(-20000.0L) == 0.0L);
    check("expm1l(-inf) is -1", expm1l(-inf) == -1.0L);
    check("asinl and acosl are NaN outside [-1, 1]",
          isnan(asinl(1.5L)) != 0 && isnan(acosl(-1.5L)) != 0);
    check("acoshl is NaN below one and atanhl is NaN above one",
          isnan(acoshl(0.5L)) != 0 && isnan(atanhl(1.5L)) != 0);
    check("atanhl(1) is infinite and atanhl(-1) is negative infinity",
          atanhl(1.0L) == inf && atanhl(-1.0L) == -inf);
    check("hypotl with an infinite argument is infinite even beside a NaN",
          hypotl(inf, nan) == inf);
    check("fmodl by zero is NaN and fmodl by an infinity is the dividend",
          isnan(fmodl(1.0L, 0.0L)) != 0 && fmodl(3.0L, inf) == 3.0L);
    check("fmaxl and fminl ignore a NaN argument",
          fmaxl(nan, 2.0L) == 2.0L && fminl(nan, 2.0L) == 2.0L);
    check("fdiml is the positive difference or zero",
          fdiml(5.0L, 2.0L) == 3.0L && fdiml(2.0L, 5.0L) == 0.0L);
    check("ilogbl reports the standard's answers for zero and NaN",
          ilogbl(0.0L) == FP_ILOGB0 && ilogbl(nan) == FP_ILOGBNAN);
    check("copysignl moves the sign of a negative zero",
          copysignl(1.0L, -0.0L) == -1.0L);
    check("sinl, cosl and tanl refuse an argument the FPU cannot reduce",
          isnan(sinl(inf)) != 0 && isnan(cosl(1e30L)) != 0 &&
          isnan(tanl(nan)) != 0);
    check("atan2l keeps the quadrant the signs of its arguments name",
          atan2l(1.0L, 1.0L) > 0.78L && atan2l(1.0L, 1.0L) < 0.79L &&
          atan2l(1.0L, -1.0L) > 2.35L && atan2l(-1.0L, -1.0L) < -2.35L);

    /* The extended format is not double with room to spare: a value this
       library must hold is one the other two types cannot express at all. */
    long double beyond_double = nextafterl(1.0L, 2.0L);
    check("a long double one step above 1 is 1 when narrowed to a double",
          beyond_double != 1.0L && (double)beyond_double == 1.0 &&
          sizeof(long double) == 16 && __LDBL_MANT_DIG__ == 64);
}

int main(void) {
    printf("mathltest: grading %u functions against GCC's own MPFR, folded "
           "at 64 bits of mantissa\n", LONG_DOUBLE_FUNCTION_COUNT);
    grade_table();
    grade_rounding_mode();
    grade_identities();
    grade_special_values();

    if (failures != 0) {
        printf("mathltest: FAILED - %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("[m142] a long double library for the x87's own format: %d checks "
           "over %u functions, every swept answer from MPFR at this target's "
           "precision, and none of it reachable from a double.\n",
           checks, LONG_DOUBLE_FUNCTION_COUNT);
    printf("mathltest: done\n");
    return 0;
}
