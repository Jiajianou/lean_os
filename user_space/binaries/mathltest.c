#include <fenv.h>
#include <limits.h>
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

/* The edges tools/math-test.sh grades by the clause that decides them,
   for the functions built on this long double half: fdim, ilogb and lrint
   are fdiml, ilogbl and lrintl cast down. On an x86_64 Mac that harness
   links this half and grades them; on an arm64 one it cannot run x87 code,
   stands the host's long double libm in for it and grades none of those
   answers. So the machine grades them, whichever Mac built the image.

   fdim of equal infinities is +0 and raises nothing - x > y is false, so
   7.12.12.1 performs no subtraction (an x86_64 Mac's own fdim raises
   invalid there). ilogb at a zero or an infinity raises invalid (C23
   F.10.3.8) and returns FP_ILOGB0 or INT_MAX (7.12.6.5). lrint and llrint
   of an infinity raise invalid (F.10.6.5). */
static volatile double edge_inf_d = INFINITY, edge_zero_d = 0.0;
static volatile float edge_inf_f = INFINITY, edge_zero_f = 0.0f;
static volatile long double edge_inf_l = INFINITY, edge_zero_l = 0.0L;

#define DECIDED(invalid_required, call, value_right)                          \
    do {                                                                      \
        feclearexcept(FE_ALL_EXCEPT);                                         \
        volatile __typeof__(call) decided_result = (call);                    \
        int decided_invalid = fetestexcept(FE_INVALID) != 0;                  \
        __typeof__(call) r = decided_result;                                  \
        check(#call " raises what the standard says and returns its value",   \
              decided_invalid == (invalid_required) && (value_right));        \
    } while (0)

static void grade_decided_edges(void) {
    DECIDED(0, fdim(edge_inf_d, edge_inf_d), r == 0.0 && !signbit(r));
    DECIDED(0, fdim(-edge_inf_d, -edge_inf_d), r == 0.0 && !signbit(r));
    DECIDED(0, fdimf(edge_inf_f, edge_inf_f), r == 0.0f && !signbit(r));
    DECIDED(0, fdimf(-edge_inf_f, -edge_inf_f), r == 0.0f && !signbit(r));
    DECIDED(0, fdiml(edge_inf_l, edge_inf_l), r == 0.0L && !signbit(r));
    DECIDED(0, fdiml(-edge_inf_l, -edge_inf_l), r == 0.0L && !signbit(r));
    DECIDED(1, ilogb(edge_zero_d), r == FP_ILOGB0);
    DECIDED(1, ilogb(-edge_zero_d), r == FP_ILOGB0);
    DECIDED(1, ilogb(edge_inf_d), r == INT_MAX);
    DECIDED(1, ilogb(-edge_inf_d), r == INT_MAX);
    DECIDED(1, ilogbf(edge_zero_f), r == FP_ILOGB0);
    DECIDED(1, ilogbf(edge_inf_f), r == INT_MAX);
    DECIDED(1, ilogbl(edge_zero_l), r == FP_ILOGB0);
    DECIDED(1, ilogbl(edge_inf_l), r == INT_MAX);
    DECIDED(1, lrint(edge_inf_d), ((void)r, 1));
    DECIDED(1, llrint(-edge_inf_d), ((void)r, 1));
    DECIDED(1, lrintf(edge_inf_f), ((void)r, 1));
    DECIDED(1, llrintf(-edge_inf_f), ((void)r, 1));
    DECIDED(1, lrintl(edge_inf_l), ((void)r, 1));
    DECIDED(1, llrintl(-edge_inf_l), ((void)r, 1));
}
#undef DECIDED

/* A quiet NaN through every function <math.h> declares, in all three
   formats, asking what each RAISED - on this machine, from the code
   x86_64-elf-gcc built, which is the code that ships.

   That last part is why this is here rather than only in tools/math-test.sh.
   C's x < y is a signaling comparison, and this compiler emits COMISD for it
   (fcomi for a long double), which raises FE_INVALID for a quiet NaN - so
   `if (x < 0.0)` in front of a NaN made asin(NaN), sqrt(NaN), fabs(NaN) and
   others report a domain error that was never there. The host test compiles
   the same source with the host's compiler, and whether a comparison
   signals is the compiler's decision: Apple's clang on arm64 emits a quiet
   fcmp and that host had never seen it. Only the shipped object answers for
   the shipped object.

   C11 Annex F decides each answer. A NaN argument is not an error and
   raises nothing, except where the result cannot be a NaN: the integer
   roundings (F.10.6.5, F.10.6.7) and ilogb (IEEE 754-2008 5.3.3, C23
   F.10.3.8) REQUIRE invalid, and are graded on raising it. Where Annex F
   gives a NaN argument a number instead - pow(NaN, 0), pow(1, NaN),
   hypot(inf, NaN), fmax and fmin - that number is checked too.

   Each probe is one line naming the function once, then its argument list:
   QUIET(f, (arguments)) is f arguments, and the macro writes the call and
   the label from that one name. A quiet NaN reaches an argument list only
   through NAN_D, NAN_F or NAN_L, which count their reads - so a probe whose
   arguments hold no NaN fails here, as that, instead of passing as a
   function that raised nothing. And because the name and the arguments are
   fields, tools/math-long-double-test.sh can check mechanically that every
   function <math.h> declares has a probe whose arguments hold a quiet NaN -
   not merely a call somewhere in this function - and checks that the check
   fails when one is taken away. */
static volatile double quiet_nan_d, zero_d = 0.0, one_d = 1.0, two_d = 2.0;
static volatile float quiet_nan_f, zero_f = 0.0f, one_f = 1.0f, two_f = 2.0f;
static volatile long double quiet_nan_l, zero_l = 0.0L, one_l = 1.0L,
                            two_l = 2.0L;
static volatile double inf_d;
static volatile float inf_f;
static volatile long double inf_l;
static int quiet_checks, quiet_failures, quiet_nan_reads;

#define NAN_D (quiet_nan_reads++, quiet_nan_d)
#define NAN_F (quiet_nan_reads++, quiet_nan_f)
#define NAN_L (quiet_nan_reads++, quiet_nan_l)

static void grade_quiet(const char *call, int raised_invalid,
                        int invalid_required, int value_right,
                        int had_quiet_nan) {
    quiet_checks++;
    if ((raised_invalid != 0) == (invalid_required != 0) && value_right &&
        had_quiet_nan) {
        return;
    }
    quiet_failures++;
    if (quiet_failures > 60) {
        return;
    }
    if (!had_quiet_nan) {
        printf("mathltest: FAIL %s is a quiet NaN probe with no quiet NaN "
               "among its arguments - it can only ever pass\n", call);
    } else if ((raised_invalid != 0) != (invalid_required != 0)) {
        printf("mathltest: FAIL %s raised %s, where Annex F %s\n", call,
               raised_invalid ? "FE_INVALID" : "nothing",
               invalid_required ? "requires invalid"
                                : "says a quiet NaN raises nothing");
    } else {
        printf("mathltest: FAIL %s returned the wrong value\n", call);
    }
}

/* Volatile in, volatile out: a call on a constant would be folded, and one
   whose result is unused could be dropped - and either raises nothing. The
   label is stringified by the macro the line names, so it reads fabs(NAN_D)
   rather than what NAN_D expands to. */
#define PROBE(label, invalid_required, function, arguments, value_right)     \
    do {                                                                     \
        int reads_before = quiet_nan_reads;                                  \
        feclearexcept(FE_ALL_EXCEPT);                                        \
        volatile __typeof__(function arguments) probe_result =               \
            function arguments;                                              \
        int probe_invalid = fetestexcept(FE_INVALID);                        \
        __typeof__(function arguments) r = probe_result;                     \
        grade_quiet(label, probe_invalid, (invalid_required),                \
                    (value_right), quiet_nan_reads != reads_before);         \
    } while (0)
/* Raises nothing and returns a NaN; raises nothing and returns what
   value_right says; raises invalid; raises invalid and returns what
   value_right says. */
#define QUIET(function, arguments)                                           \
    PROBE(#function #arguments, 0, function, arguments, isnan(r))
#define EXACT(function, arguments, value_right)                              \
    PROBE(#function #arguments, 0, function, arguments, value_right)
#define LOUD(function, arguments)                                            \
    PROBE(#function #arguments, 1, function, arguments, ((void)r, 1))
#define LOUD_EXACT(function, arguments, value_right)                         \
    PROBE(#function #arguments, 1, function, arguments, value_right)
/* The probe's own premise, which is the one call here with no NaN in it:
   sqrt(-1) must raise invalid and return a NaN, or nothing above could have
   failed. */
#define PREMISE(function, arguments)                                         \
    do {                                                                     \
        feclearexcept(FE_ALL_EXCEPT);                                        \
        volatile __typeof__(function arguments) premise_result =             \
            function arguments;                                              \
        int premise_invalid = fetestexcept(FE_INVALID);                      \
        grade_quiet(#function #arguments, premise_invalid, 1,                \
                    isnan(premise_result), 1);                               \
    } while (0)

static void grade_quiet_nan(void) {
    quiet_nan_d = (double)NAN;
    quiet_nan_f = NAN;
    quiet_nan_l = (long double)NAN;
    inf_d = (double)INFINITY;
    inf_f = INFINITY;
    inf_l = (long double)INFINITY;
    int exponent = 0, quotient = 0;
    double whole_d = 0.0;
    float whole_f = 0.0f;
    long double whole_l = 0.0L;

    QUIET(fabs, (NAN_D));
    QUIET(sqrt, (NAN_D));
    QUIET(floor, (NAN_D));
    QUIET(ceil, (NAN_D));
    QUIET(trunc, (NAN_D));
    QUIET(round, (NAN_D));
    QUIET(nearbyint, (NAN_D));
    QUIET(rint, (NAN_D));
    QUIET(sin, (NAN_D));
    QUIET(cos, (NAN_D));
    QUIET(tan, (NAN_D));
    QUIET(asin, (NAN_D));
    QUIET(acos, (NAN_D));
    QUIET(atan, (NAN_D));
    QUIET(exp, (NAN_D));
    QUIET(exp2, (NAN_D));
    QUIET(expm1, (NAN_D));
    QUIET(log, (NAN_D));
    QUIET(log10, (NAN_D));
    QUIET(log2, (NAN_D));
    QUIET(log1p, (NAN_D));
    QUIET(logb, (NAN_D));
    QUIET(sinh, (NAN_D));
    QUIET(cosh, (NAN_D));
    QUIET(tanh, (NAN_D));
    QUIET(asinh, (NAN_D));
    QUIET(acosh, (NAN_D));
    QUIET(atanh, (NAN_D));
    QUIET(erf, (NAN_D));
    QUIET(erfc, (NAN_D));
    QUIET(tgamma, (NAN_D));
    QUIET(lgamma, (NAN_D));
    QUIET(cbrt, (NAN_D));
    QUIET(frexp, (NAN_D, &exponent));
    QUIET(ldexp, (NAN_D, 3));
    QUIET(scalbn, (NAN_D, 3));
    QUIET(scalbln, (NAN_D, 3L));
    EXACT(modf, (NAN_D, &whole_d), isnan(r) && isnan(whole_d));
    QUIET(atan2, (NAN_D, one_d));
    QUIET(atan2, (one_d, NAN_D));
    QUIET(pow, (NAN_D, two_d));
    QUIET(pow, (two_d, NAN_D));
    EXACT(pow, (NAN_D, zero_d), r == 1.0);
    EXACT(pow, (one_d, NAN_D), r == 1.0);
    QUIET(fmod, (NAN_D, one_d));
    QUIET(fmod, (one_d, NAN_D));
    QUIET(remainder, (NAN_D, one_d));
    QUIET(remainder, (one_d, NAN_D));
    QUIET(remquo, (NAN_D, one_d, &quotient));
    QUIET(remquo, (one_d, NAN_D, &quotient));
    QUIET(hypot, (NAN_D, one_d));
    EXACT(hypot, (inf_d, NAN_D), isinf(r));
    EXACT(hypot, (NAN_D, inf_d), isinf(r));
    QUIET(copysign, (NAN_D, one_d));
    EXACT(copysign, (two_d, NAN_D), fabs(r) == 2.0);
    QUIET(nextafter, (NAN_D, one_d));
    QUIET(nextafter, (one_d, NAN_D));
    QUIET(nexttoward, (NAN_D, one_l));
    QUIET(nexttoward, (one_d, NAN_L));
    QUIET(fdim, (NAN_D, one_d));
    QUIET(fdim, (one_d, NAN_D));
    EXACT(fmax, (NAN_D, two_d), r == 2.0);
    EXACT(fmax, (two_d, NAN_D), r == 2.0);
    EXACT(fmin, (NAN_D, two_d), r == 2.0);
    EXACT(fmin, (two_d, NAN_D), r == 2.0);
    QUIET(fma, (NAN_D, one_d, one_d));
    QUIET(fma, (one_d, NAN_D, one_d));
    QUIET(fma, (one_d, one_d, NAN_D));
    LOUD(lround, (NAN_D));
    LOUD(llround, (NAN_D));
    LOUD(lrint, (NAN_D));
    LOUD(llrint, (NAN_D));
    LOUD_EXACT(ilogb, (NAN_D), r == FP_ILOGBNAN);

    QUIET(fabsf, (NAN_F));
    QUIET(sqrtf, (NAN_F));
    QUIET(floorf, (NAN_F));
    QUIET(ceilf, (NAN_F));
    QUIET(truncf, (NAN_F));
    QUIET(roundf, (NAN_F));
    QUIET(nearbyintf, (NAN_F));
    QUIET(rintf, (NAN_F));
    QUIET(sinf, (NAN_F));
    QUIET(cosf, (NAN_F));
    QUIET(tanf, (NAN_F));
    QUIET(asinf, (NAN_F));
    QUIET(acosf, (NAN_F));
    QUIET(atanf, (NAN_F));
    QUIET(expf, (NAN_F));
    QUIET(exp2f, (NAN_F));
    QUIET(expm1f, (NAN_F));
    QUIET(logf, (NAN_F));
    QUIET(log10f, (NAN_F));
    QUIET(log2f, (NAN_F));
    QUIET(log1pf, (NAN_F));
    QUIET(logbf, (NAN_F));
    QUIET(sinhf, (NAN_F));
    QUIET(coshf, (NAN_F));
    QUIET(tanhf, (NAN_F));
    QUIET(asinhf, (NAN_F));
    QUIET(acoshf, (NAN_F));
    QUIET(atanhf, (NAN_F));
    QUIET(erff, (NAN_F));
    QUIET(erfcf, (NAN_F));
    QUIET(tgammaf, (NAN_F));
    QUIET(lgammaf, (NAN_F));
    QUIET(cbrtf, (NAN_F));
    QUIET(frexpf, (NAN_F, &exponent));
    QUIET(ldexpf, (NAN_F, 3));
    QUIET(scalbnf, (NAN_F, 3));
    QUIET(scalblnf, (NAN_F, 3L));
    EXACT(modff, (NAN_F, &whole_f), isnan(r) && isnan(whole_f));
    QUIET(atan2f, (NAN_F, one_f));
    QUIET(atan2f, (one_f, NAN_F));
    QUIET(powf, (NAN_F, two_f));
    QUIET(powf, (two_f, NAN_F));
    EXACT(powf, (NAN_F, zero_f), r == 1.0f);
    EXACT(powf, (one_f, NAN_F), r == 1.0f);
    QUIET(fmodf, (NAN_F, one_f));
    QUIET(fmodf, (one_f, NAN_F));
    QUIET(remainderf, (NAN_F, one_f));
    QUIET(remainderf, (one_f, NAN_F));
    QUIET(remquof, (NAN_F, one_f, &quotient));
    QUIET(remquof, (one_f, NAN_F, &quotient));
    QUIET(hypotf, (NAN_F, one_f));
    EXACT(hypotf, (inf_f, NAN_F), isinf(r));
    QUIET(copysignf, (NAN_F, one_f));
    EXACT(copysignf, (two_f, NAN_F), fabsf(r) == 2.0f);
    QUIET(nextafterf, (NAN_F, one_f));
    QUIET(nextafterf, (one_f, NAN_F));
    QUIET(nexttowardf, (NAN_F, one_l));
    QUIET(nexttowardf, (one_f, NAN_L));
    QUIET(fdimf, (NAN_F, one_f));
    QUIET(fdimf, (one_f, NAN_F));
    EXACT(fmaxf, (NAN_F, two_f), r == 2.0f);
    EXACT(fmaxf, (two_f, NAN_F), r == 2.0f);
    EXACT(fminf, (NAN_F, two_f), r == 2.0f);
    EXACT(fminf, (two_f, NAN_F), r == 2.0f);
    QUIET(fmaf, (NAN_F, one_f, one_f));
    QUIET(fmaf, (one_f, NAN_F, one_f));
    QUIET(fmaf, (one_f, one_f, NAN_F));
    LOUD(lroundf, (NAN_F));
    LOUD(llroundf, (NAN_F));
    LOUD(lrintf, (NAN_F));
    LOUD(llrintf, (NAN_F));
    LOUD_EXACT(ilogbf, (NAN_F), r == FP_ILOGBNAN);

    QUIET(fabsl, (NAN_L));
    QUIET(sqrtl, (NAN_L));
    QUIET(floorl, (NAN_L));
    QUIET(ceill, (NAN_L));
    QUIET(truncl, (NAN_L));
    QUIET(roundl, (NAN_L));
    QUIET(nearbyintl, (NAN_L));
    QUIET(rintl, (NAN_L));
    QUIET(sinl, (NAN_L));
    QUIET(cosl, (NAN_L));
    QUIET(tanl, (NAN_L));
    QUIET(asinl, (NAN_L));
    QUIET(acosl, (NAN_L));
    QUIET(atanl, (NAN_L));
    QUIET(expl, (NAN_L));
    QUIET(exp2l, (NAN_L));
    QUIET(expm1l, (NAN_L));
    QUIET(logl, (NAN_L));
    QUIET(log10l, (NAN_L));
    QUIET(log2l, (NAN_L));
    QUIET(log1pl, (NAN_L));
    QUIET(logbl, (NAN_L));
    QUIET(sinhl, (NAN_L));
    QUIET(coshl, (NAN_L));
    QUIET(tanhl, (NAN_L));
    QUIET(asinhl, (NAN_L));
    QUIET(acoshl, (NAN_L));
    QUIET(atanhl, (NAN_L));
    QUIET(cbrtl, (NAN_L));
    QUIET(frexpl, (NAN_L, &exponent));
    QUIET(ldexpl, (NAN_L, 3));
    QUIET(scalbnl, (NAN_L, 3));
    QUIET(scalblnl, (NAN_L, 3L));
    EXACT(modfl, (NAN_L, &whole_l), isnan(r) && isnan(whole_l));
    QUIET(atan2l, (NAN_L, one_l));
    QUIET(atan2l, (one_l, NAN_L));
    QUIET(powl, (NAN_L, two_l));
    QUIET(powl, (two_l, NAN_L));
    EXACT(powl, (NAN_L, zero_l), r == 1.0L);
    EXACT(powl, (one_l, NAN_L), r == 1.0L);
    QUIET(fmodl, (NAN_L, one_l));
    QUIET(fmodl, (one_l, NAN_L));
    QUIET(remainderl, (NAN_L, one_l));
    QUIET(remainderl, (one_l, NAN_L));
    QUIET(remquol, (NAN_L, one_l, &quotient));
    QUIET(remquol, (one_l, NAN_L, &quotient));
    QUIET(hypotl, (NAN_L, one_l));
    EXACT(hypotl, (inf_l, NAN_L), isinf(r));
    QUIET(copysignl, (NAN_L, one_l));
    EXACT(copysignl, (two_l, NAN_L), fabsl(r) == 2.0L);
    QUIET(nextafterl, (NAN_L, one_l));
    QUIET(nextafterl, (one_l, NAN_L));
    QUIET(nexttowardl, (NAN_L, one_l));
    QUIET(nexttowardl, (one_l, NAN_L));
    QUIET(fdiml, (NAN_L, one_l));
    QUIET(fdiml, (one_l, NAN_L));
    EXACT(fmaxl, (NAN_L, two_l), r == 2.0L);
    EXACT(fmaxl, (two_l, NAN_L), r == 2.0L);
    EXACT(fminl, (NAN_L, two_l), r == 2.0L);
    EXACT(fminl, (two_l, NAN_L), r == 2.0L);
    LOUD(lroundl, (NAN_L));
    LOUD(llroundl, (NAN_L));
    LOUD(lrintl, (NAN_L));
    LOUD(llrintl, (NAN_L));
    LOUD_EXACT(ilogbl, (NAN_L), r == FP_ILOGBNAN);

    /* The probe's own premise: the flag is readable at all, and the NaN it
       hands out is quiet. A machine where nothing could raise would pass
       every QUIET above and fail only here. */
    PREMISE(sqrt, (-one_d));
    PREMISE(sqrtf, (-one_f));
    PREMISE(sqrtl, (-one_l));
    feclearexcept(FE_ALL_EXCEPT);
    volatile double sum = quiet_nan_d + one_d;
    (void)sum;
    if (fetestexcept(FE_INVALID)) {
        printf("mathltest: FAIL the probe's NaN is signaling - arithmetic on "
               "it raised invalid\n");
        quiet_failures++;
    }
    quiet_checks++;
}
#undef PREMISE
#undef LOUD_EXACT
#undef LOUD
#undef EXACT
#undef QUIET
#undef PROBE
#undef NAN_L
#undef NAN_F
#undef NAN_D

int main(void) {
    printf("mathltest: grading %u functions against GCC's own MPFR, folded "
           "at 64 bits of mantissa\n", LONG_DOUBLE_FUNCTION_COUNT);
    grade_quiet_nan();
    grade_table();
    grade_rounding_mode();
    grade_identities();
    grade_special_values();
    grade_decided_edges();

    if (quiet_failures == 0) {
        printf("[m142q] a quiet NaN through every function <math.h> declares: "
               "%d calls in double, float and long double, compiled by the "
               "compiler that ships, invalid raised only where Annex F "
               "requires it.\n", quiet_checks);
    } else {
        printf("mathltest: FAILED - %d of %d quiet NaN checks\n",
               quiet_failures, quiet_checks);
    }
    if (failures != 0) {
        printf("mathltest: FAILED - %d of %d checks\n", failures, checks);
    }
    /* Which half failed is the exit status, so the M142 panic can say
       which: 1 for the long double library's own checks, 2 for the quiet
       NaN probe, 3 for both. */
    if (failures != 0 || quiet_failures != 0) {
        return (failures != 0 ? 1 : 0) | (quiet_failures != 0 ? 2 : 0);
    }
    printf("[m142] a long double library for the x87's own format: %d checks "
           "over %u functions, every swept answer from MPFR at this target's "
           "precision, and none of it reachable from a double.\n",
           checks, LONG_DOUBLE_FUNCTION_COUNT);
    printf("mathltest: done\n");
    return 0;
}
