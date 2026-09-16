#include <fenv.h>
#include <math.h>
#include <stdio.h>
/* Without this, a compiler is allowed to move a floating point operation
   across a call that inspects the flags - which is exactly what the table at
   the bottom of this file does, and with -O2 the host's sqrt(-1) was being
   hoisted above feclearexcept and its FE_INVALID wiped before it was read.
   The measurement then said the host's libm raises nothing, which is a
   sentence about this harness rather than about that library. */
#pragma STDC FENV_ACCESS ON
#include <stdlib.h>
#include <string.h>

double lean_fabs(double), lean_sqrt(double), lean_floor(double);
double lean_ceil(double), lean_trunc(double), lean_round(double);
double lean_sin(double), lean_cos(double), lean_tan(double);
double lean_atan(double), lean_asin(double), lean_acos(double);
double lean_exp(double), lean_log(double), lean_log10(double), lean_log2(double);
double lean_sinh(double), lean_cosh(double), lean_tanh(double);
double lean_expm1(double), lean_log1p(double);
double lean_asinh(double), lean_acosh(double), lean_atanh(double);
double lean_erf(double), lean_erfc(double);
double lean_cbrt(double), lean_exp2(double);
double lean_fma(double, double, double);
double lean_pow(double, double), lean_atan2(double, double);
double lean_fmod(double, double), lean_hypot(double, double);
double lean_copysign(double, double), lean_nextafter(double, double);
double lean_fmax(double, double), lean_fmin(double, double);
float lean_fabsf(float), lean_floorf(float), lean_ceilf(float);
float lean_sinf(float), lean_cosf(float), lean_tanf(float);
float lean_hypotf(float, float);
float lean_sqrtf(float), lean_atanf(float), lean_roundf(float);
float lean_expf(float);
float lean_tanhf(float);
double lean_nan(const char *);
float lean_nanf(const char *);
float lean_nextafterf(float, float);
float lean_atan2f(float, float);
float lean_ldexpf(float, int);
long lean_lround(double), lean_lroundf(float);
long long lean_llround(double), lean_llroundf(float);

/* This file speaks two dialects of <fenv.h> and has to translate between
   them. The library under test is compiled against THIS PROJECT's headers,
   where the exception flags have the x87's own bit layout, and it is linked
   against the host's libm, whose feraiseexcept takes the host's. So a
   domain error raised by this libm arrives in the host's flag word at the
   bit this project's FE_INVALID names, and the two words have to be read
   with different tables. Getting this wrong is quiet rather than loud: the
   first run of this table reported that log(0) raises FE_OVERFLOW, which is
   this project's FE_DIVBYZERO (0x04) read as the host's FE_OVERFLOW (0x04).
   Both libraries were right and the reader was wrong.

   The values are the ones in user_space/libc/include/fenv.h. They are spelled
   out rather than included because the host's <fenv.h> is already in scope
   here and one translation unit cannot have both. */
#define LEAN_FE_INVALID   0x01
#define LEAN_FE_DIVBYZERO 0x04
#define LEAN_FE_OVERFLOW  0x08
#define LEAN_FE_ALL       0x3F

/* Everything this table compares, in the one vocabulary the comparison can
   use. FE_INEXACT accompanies an overflow in both libraries and is not among
   them, because whether a library sets it alongside is not what
   math_errhandling promises. */
enum {
    ERROR_INVALID = 1,
    ERROR_DIVBYZERO = 2,
    ERROR_OVERFLOW = 4,
};

static int host_errors(int flags) {
    return (flags & FE_INVALID ? ERROR_INVALID : 0) |
           (flags & FE_DIVBYZERO ? ERROR_DIVBYZERO : 0) |
           (flags & FE_OVERFLOW ? ERROR_OVERFLOW : 0);
}

static int lean_errors(int flags) {
    return (flags & LEAN_FE_INVALID ? ERROR_INVALID : 0) |
           (flags & LEAN_FE_DIVBYZERO ? ERROR_DIVBYZERO : 0) |
           (flags & LEAN_FE_OVERFLOW ? ERROR_OVERFLOW : 0);
}

/* Names for a diagnostic, so a disagreement says which flag rather than
   which bit pattern. */
static const char *flag_names(int flags) {
    /* Two buffers in rotation, because both arguments of one printf are
       evaluated before it runs and a single static buffer would hand it the
       same string twice. */
    static char buffers[2][64];
    static int which = 0;
    char *text = buffers[which];
    which = 1 - which;
    text[0] = '\0';
    if (flags & ERROR_INVALID) {
        strcat(text, "FE_INVALID ");
    }
    if (flags & ERROR_DIVBYZERO) {
        strcat(text, "FE_DIVBYZERO ");
    }
    if (flags & ERROR_OVERFLOW) {
        strcat(text, "FE_OVERFLOW ");
    }
    if (text[0] == '\0') {
        return "nothing";
    }
    text[strlen(text) - 1] = '\0';
    return text;
}


/* M155. The rest of C99 7.12. ilogb returns an int and this harness's
   integer kind takes a long, so both sides go through a wrapper rather than
   the table growing a shape for one function. */
double lean_nearbyint(double), lean_rint(double), lean_logb(double);
long lean_lrint(double);
long long lean_llrint(double);
int lean_ilogb(double);
double lean_remainder(double, double), lean_fdim(double, double);
float lean_acosf(float), lean_asinf(float), lean_acoshf(float);
float lean_asinhf(float), lean_atanhf(float), lean_coshf(float);
float lean_sinhf(float), lean_exp2f(float), lean_expm1f(float);
float lean_logf(float), lean_log10f(float), lean_log1pf(float);
float lean_log2f(float), lean_logbf(float), lean_cbrtf(float);
float lean_erff(float), lean_erfcf(float), lean_nearbyintf(float);
float lean_rintf(float), lean_truncf(float);
float lean_powf(float, float), lean_fmodf(float, float);
float lean_remainderf(float, float), lean_copysignf(float, float);
float lean_fdimf(float, float), lean_fmaxf(float, float);
float lean_fminf(float, float);
long lean_lrintf(float);
long long lean_llrintf(float);
float lean_scalbnf(float, int);

static long lean_ilogb_long(double x) { return lean_ilogb(x); }
static long host_ilogb_long(double x) { return ilogb(x); }

typedef double (*fn1)(double);
typedef double (*fn2)(double, double);
typedef double (*fn3)(double, double, double);
typedef float (*fn1f)(float);
typedef float (*fn2f)(float, float);
typedef float (*fnfi)(float, int);
typedef long (*fnl)(double);
typedef long (*fnlf)(float);
typedef long long (*fnll)(double);
typedef long long (*fnllf)(float);

struct entry {
    const char *name;
    fn1 ours1, theirs1;
    fn2 ours2, theirs2;
    fn3 ours3, theirs3;
    fn1f oursf, theirsf;
    fn2f oursF, theirsF;
    fnfi oursfi, theirsfi;
    fnl oursl, theirsl;
    fnlf ourslf, theirslf;
    fnll oursll, theirsll;
    fnllf oursllf, theirsllf;
};

static const struct entry TABLE[] = {
    {.name = "nearbyint", .ours1 = lean_nearbyint, .theirs1 = nearbyint},
    {.name = "rint", .ours1 = lean_rint, .theirs1 = rint},
    {.name = "logb", .ours1 = lean_logb, .theirs1 = logb},
    {.name = "lrint", .oursl = lean_lrint, .theirsl = lrint},
    {.name = "llrint", .oursll = lean_llrint, .theirsll = llrint},
    {.name = "ilogb", .oursl = lean_ilogb_long, .theirsl = host_ilogb_long},
    {.name = "remainder", .ours2 = lean_remainder, .theirs2 = remainder},
    {.name = "fdim", .ours2 = lean_fdim, .theirs2 = fdim},
    {.name = "acosf", .oursf = lean_acosf, .theirsf = acosf},
    {.name = "asinf", .oursf = lean_asinf, .theirsf = asinf},
    {.name = "acoshf", .oursf = lean_acoshf, .theirsf = acoshf},
    {.name = "asinhf", .oursf = lean_asinhf, .theirsf = asinhf},
    {.name = "atanhf", .oursf = lean_atanhf, .theirsf = atanhf},
    {.name = "coshf", .oursf = lean_coshf, .theirsf = coshf},
    {.name = "sinhf", .oursf = lean_sinhf, .theirsf = sinhf},
    {.name = "exp2f", .oursf = lean_exp2f, .theirsf = exp2f},
    {.name = "expm1f", .oursf = lean_expm1f, .theirsf = expm1f},
    {.name = "logf", .oursf = lean_logf, .theirsf = logf},
    {.name = "log10f", .oursf = lean_log10f, .theirsf = log10f},
    {.name = "log1pf", .oursf = lean_log1pf, .theirsf = log1pf},
    {.name = "log2f", .oursf = lean_log2f, .theirsf = log2f},
    {.name = "logbf", .oursf = lean_logbf, .theirsf = logbf},
    {.name = "cbrtf", .oursf = lean_cbrtf, .theirsf = cbrtf},
    {.name = "erff", .oursf = lean_erff, .theirsf = erff},
    {.name = "erfcf", .oursf = lean_erfcf, .theirsf = erfcf},
    {.name = "nearbyintf", .oursf = lean_nearbyintf, .theirsf = nearbyintf},
    {.name = "rintf", .oursf = lean_rintf, .theirsf = rintf},
    {.name = "truncf", .oursf = lean_truncf, .theirsf = truncf},
    {.name = "powf", .oursF = lean_powf, .theirsF = powf},
    {.name = "fmodf", .oursF = lean_fmodf, .theirsF = fmodf},
    {.name = "remainderf", .oursF = lean_remainderf, .theirsF = remainderf},
    {.name = "copysignf", .oursF = lean_copysignf, .theirsF = copysignf},
    {.name = "fdimf", .oursF = lean_fdimf, .theirsF = fdimf},
    {.name = "fmaxf", .oursF = lean_fmaxf, .theirsF = fmaxf},
    {.name = "fminf", .oursF = lean_fminf, .theirsF = fminf},
    {.name = "lrintf", .ourslf = lean_lrintf, .theirslf = lrintf},
    {.name = "llrintf", .oursllf = lean_llrintf, .theirsllf = llrintf},
    {.name = "scalbnf", .oursfi = lean_scalbnf, .theirsfi = scalbnf},
    {.name = "fabsf", .oursf = lean_fabsf, .theirsf = fabsf},
    {.name = "floorf", .oursf = lean_floorf, .theirsf = floorf},
    {.name = "ceilf", .oursf = lean_ceilf, .theirsf = ceilf},
    {.name = "sinf", .oursf = lean_sinf, .theirsf = sinf},
    {.name = "cosf", .oursf = lean_cosf, .theirsf = cosf},
    {.name = "tanf", .oursf = lean_tanf, .theirsf = tanf},
    {.name = "hypotf", .oursF = lean_hypotf, .theirsF = hypotf},
    {.name = "sqrtf", .oursf = lean_sqrtf, .theirsf = sqrtf},
    {.name = "atanf", .oursf = lean_atanf, .theirsf = atanf},
    {.name = "roundf", .oursf = lean_roundf, .theirsf = roundf},
    {.name = "fabs", .ours1 = lean_fabs, .theirs1 = fabs},
    {.name = "sqrt", .ours1 = lean_sqrt, .theirs1 = sqrt},
    {.name = "floor", .ours1 = lean_floor, .theirs1 = floor},
    {.name = "ceil", .ours1 = lean_ceil, .theirs1 = ceil},
    {.name = "trunc", .ours1 = lean_trunc, .theirs1 = trunc},
    {.name = "round", .ours1 = lean_round, .theirs1 = round},
    {.name = "sin", .ours1 = lean_sin, .theirs1 = sin},
    {.name = "cos", .ours1 = lean_cos, .theirs1 = cos},
    {.name = "tan", .ours1 = lean_tan, .theirs1 = tan},
    {.name = "atan", .ours1 = lean_atan, .theirs1 = atan},
    {.name = "asin", .ours1 = lean_asin, .theirs1 = asin},
    {.name = "acos", .ours1 = lean_acos, .theirs1 = acos},
    {.name = "exp", .ours1 = lean_exp, .theirs1 = exp},
    {.name = "log", .ours1 = lean_log, .theirs1 = log},
    {.name = "log10", .ours1 = lean_log10, .theirs1 = log10},
    {.name = "log2", .ours1 = lean_log2, .theirs1 = log2},
    {.name = "sinh", .ours1 = lean_sinh, .theirs1 = sinh},
    {.name = "cosh", .ours1 = lean_cosh, .theirs1 = cosh},
    {.name = "tanh", .ours1 = lean_tanh, .theirs1 = tanh},
    {.name = "expm1", .ours1 = lean_expm1, .theirs1 = expm1},
    {.name = "log1p", .ours1 = lean_log1p, .theirs1 = log1p},
    {.name = "asinh", .ours1 = lean_asinh, .theirs1 = asinh},
    {.name = "acosh", .ours1 = lean_acosh, .theirs1 = acosh},
    {.name = "atanh", .ours1 = lean_atanh, .theirs1 = atanh},
    {.name = "erf", .ours1 = lean_erf, .theirs1 = erf},
    {.name = "erfc", .ours1 = lean_erfc, .theirs1 = erfc},
    {.name = "pow", .ours2 = lean_pow, .theirs2 = pow},
    {.name = "atan2", .ours2 = lean_atan2, .theirs2 = atan2},
    {.name = "fmod", .ours2 = lean_fmod, .theirs2 = fmod},
    {.name = "hypot", .ours2 = lean_hypot, .theirs2 = hypot},
    {.name = "copysign", .ours2 = lean_copysign, .theirs2 = copysign},
    {.name = "nextafter", .ours2 = lean_nextafter, .theirs2 = nextafter},
    {.name = "expf", .oursf = lean_expf, .theirsf = expf},
    {.name = "tanhf", .oursf = lean_tanhf, .theirsf = tanhf},
    {.name = "nextafterf", .oursF = lean_nextafterf, .theirsF = nextafterf},
    {.name = "atan2f", .oursF = lean_atan2f, .theirsF = atan2f},
    {.name = "ldexpf", .oursfi = lean_ldexpf, .theirsfi = ldexpf},
    {.name = "fmax", .ours2 = lean_fmax, .theirs2 = fmax},
    {.name = "fmin", .ours2 = lean_fmin, .theirs2 = fmin},
    {.name = "cbrt", .ours1 = lean_cbrt, .theirs1 = cbrt},
    {.name = "exp2", .ours1 = lean_exp2, .theirs1 = exp2},
    {.name = "fma", .ours3 = lean_fma, .theirs3 = fma},
    {.name = "lround",   .oursl = lean_lround,     .theirsl = lround},
    {.name = "lroundf",  .ourslf = lean_lroundf,   .theirslf = lroundf},
    {.name = "llround",  .oursll = lean_llround,   .theirsll = llround},
    {.name = "llroundf", .oursllf = lean_llroundf, .theirsllf = llroundf},
};

static double error_of(double got, double want) {
    if (isnan(want)) {
        return isnan(got) ? 0.0 : 1.0;
    }
    if (isinf(want)) {
        return (isinf(got) && ((got > 0) == (want > 0))) ? 0.0 : 1.0;
    }
    if (isnan(got) || isinf(got)) {
        return 1.0;
    }
    double d = fabs(got - want);
    double scale = fabs(want);
    return scale > 1e-300 ? d / scale : d;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <cases.tsv>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }

    int failures = 0, graded = 0, skipped = 0;

    {
        static const struct { const char *name; double v; } SPECIALS[] = {
            {"+inf", 0}, {"-inf", 0}, {"nan", 0}, {"+0", 0.0}, {"-0", -0.0},
            {"1", 1.0}, {"-1", -1.0}, {"tiny", 5e-324}, {"huge", 1.7976931348623157e308},
        };
        double values[9];
        values[0] = INFINITY;
        values[1] = -INFINITY;
        values[2] = NAN;
        values[3] = 0.0;
        values[4] = -0.0;
        values[5] = 1.0;
        values[6] = -1.0;
        values[7] = 5e-324;
        values[8] = 1.7976931348623157e308;

        static const struct {
            const char *function; const char *at; const char *why;
        } DIVERGE[] = {
            {"sin", "huge", "no Payne-Hanek reduction past 2^52 - math.c refuses rather than guessing"},
            {"cos", "huge", "same"},
            {"tan", "huge", "same"},
        };

        int special_failures = 0;
        for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
            if (!TABLE[i].ours1 && !TABLE[i].oursf) {
                continue;
            }
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
                double got, want;
                if (TABLE[i].ours1) {
                    got = TABLE[i].ours1(values[v]);
                    want = TABLE[i].theirs1(values[v]);
                } else {
                    got = (double)TABLE[i].oursf((float)values[v]);
                    want = (double)TABLE[i].theirsf((float)values[v]);
                }
                double special_tol = TABLE[i].ours1 ? 1e-12 : 1.2e-7;
                if (error_of(got, want) <= special_tol) {
                    continue;
                }
                const char *why = 0;
                for (size_t d = 0; d < sizeof(DIVERGE) / sizeof(DIVERGE[0]); d++) {
                    if (strcmp(DIVERGE[d].function, TABLE[i].name) == 0 &&
                        strcmp(DIVERGE[d].at, SPECIALS[v].name) == 0) {
                        why = DIVERGE[d].why;
                        break;
                    }
                }
                if (why) {
                    if (!isnan(got)) {
                        printf("FAIL %-10s at %-5s is a known divergence but "
                               "returned %.17g rather than refusing\n",
                               TABLE[i].name, SPECIALS[v].name, got);
                        special_failures++;
                    } else {
                        printf("     %-10s at %-5s refuses: %s\n",
                               TABLE[i].name, SPECIALS[v].name, why);
                    }
                    continue;
                }
                printf("FAIL %-10s at %-5s: ours %.17g, the host's %.17g\n",
                       TABLE[i].name, SPECIALS[v].name, got, want);
                special_failures++;
            }
        }
        if (special_failures == 0) {
            printf("ok   %-10s every one-argument function at "
                   "+-inf, nan, +-0, +-1, the smallest subnormal and the "
                   "largest finite double\n", "specials");
        }
        failures += special_failures;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        char name[64], kind[8], note[256];
        double lo, hi, tol;
        int points;
        char *tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        snprintf(name, sizeof(name), "%s", line);
        if (sscanf(tab + 1, "%7s", kind) != 1) {
            continue;
        }
        if (kind[0] == '-') {
            skipped++;
            continue;
        }
        char lobuf[64], hibuf[64], ptbuf[64], tolbuf[64];
        note[0] = '\0';
        if (sscanf(tab + 1, "%7s %63s %63s %63s %63s", kind, lobuf, hibuf,
                   ptbuf, tolbuf) != 5) {
            fprintf(stderr, "math-test: malformed row for %s\n", name);
            return 2;
        }
        lo = strtod(lobuf, NULL);
        hi = strtod(hibuf, NULL);
        points = atoi(ptbuf);
        tol = strtod(tolbuf, NULL);

        const struct entry *e = NULL;
        for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
            if (strcmp(TABLE[i].name, name) == 0) {
                e = &TABLE[i];
                break;
            }
        }
        if (!e) {
            fprintf(stderr, "math-test: %s has a row but no entry in the "
                            "table in tests/math/main.c\n", name);
            return 2;
        }

        double worst = 0.0, worst_x = 0.0, worst_y = 0.0;
        double worst_got = 0.0, worst_want = 0.0;
        int n = points > 1 ? points : 2;
        double step = (hi - lo) / (double)(n - 1);
        if (kind[0] == 'f') {
            for (int i = 0; i < n; i++) {
                float x = (float)(lo + step * (double)i);
                double got = (double)e->oursf(x), want = (double)e->theirsf(x);
                double error = error_of(got, want);
                if (error > worst) {
                    worst = error; worst_x = (double)x; worst_got = got; worst_want = want;
                }
            }
        } else if (kind[0] == 'F') {
            for (int i = 0; i < n; i++) {
                float x = (float)(lo + step * (double)i);
                for (int j = 0; j < n; j++) {
                    float y = (float)(lo + step * (double)j);
                    double got = (double)e->oursF(x, y), want = (double)e->theirsF(x, y);
                    double error = error_of(got, want);
                    if (error > worst) {
                        worst = error; worst_x = (double)x; worst_y = (double)y;
                        worst_got = got; worst_want = want;
                    }
                }
            }
        } else if (kind[0] == 's') {
            /* One float and an integer exponent. ldexpf is the only shape
               like this here, and lo/hi bound the float while the exponent
               sweeps the range a float's own exponent field can hold - which
               is where scaling stops being exact and starts being a
               subnormal. */
            for (int i = 0; i < n; i++) {
                float x = (float)(lo + step * (double)i);
                for (int k = -160; k <= 160; k++) {
                    double got = (double)e->oursfi(x, k);
                    double want = (double)e->theirsfi(x, k);
                    double error = error_of(got, want);
                    if (error > worst) {
                        worst = error; worst_x = (double)x; worst_y = (double)k;
                        worst_got = got; worst_want = want;
                    }
                }
            }
        } else if (kind[0] == 'i') {
            for (int i = 0; i < n; i++) {
                double x = lo + step * (double)i;
                long long got, want;
                if (e->oursl) {
                    got = e->oursl(x); want = e->theirsl(x);
                } else if (e->ourslf) {
                    float xf = (float)x; x = (double)xf;
                    got = e->ourslf(xf); want = e->theirslf(xf);
                } else if (e->oursll) {
                    got = e->oursll(x); want = e->theirsll(x);
                } else {
                    float xf = (float)x; x = (double)xf;
                    got = e->oursllf(xf); want = e->theirsllf(xf);
                }
                if (got != want) {
                    worst = 1.0; worst_x = x;
                    worst_got = (double)got; worst_want = (double)want;
                }
            }
        } else if (kind[0] == '1') {
            for (int i = 0; i < n; i++) {
                double x = lo + step * (double)i;
                double got = e->ours1(x), want = e->theirs1(x);
                double error = error_of(got, want);
                if (error > worst) {
                    worst = error; worst_x = x; worst_got = got; worst_want = want;
                }
            }
        } else if (kind[0] == '2') {
            for (int i = 0; i < n; i++) {
                double x = lo + step * (double)i;
                for (int j = 0; j < n; j++) {
                    double y = lo + step * (double)j;
                    double got = e->ours2(x, y), want = e->theirs2(x, y);
                    double error = error_of(got, want);
                    if (error > worst) {
                        worst = error; worst_x = x; worst_y = y;
                        worst_got = got; worst_want = want;
                    }
                }
            }
        } else {
            for (int i = 0; i < n; i++) {
                double x = lo + step * (double)i;
                for (int j = 0; j < n; j++) {
                    double y = lo + step * (double)j;
                    for (int k = 0; k < n; k++) {
                        double z = lo + step * (double)k;
                        double got = e->ours3(x, y, z);
                        double want = e->theirs3(x, y, z);
                        double error = error_of(got, want);
                        if (error > worst) {
                            worst = error; worst_x = x; worst_y = y;
                            worst_got = got; worst_want = want;
                        }
                    }
                }
            }
        }
        graded++;
        if (worst > tol) {
            failures++;
            if (kind[0] == '1' || kind[0] == 'f' || kind[0] == 'i') {
                printf("FAIL %-10s worst %.3g > %.3g at x=%.17g: "
                       "ours %.17g, the host's %.17g\n",
                       name, worst, tol, worst_x, worst_got, worst_want);
            } else {
                printf("FAIL %-10s worst %.3g > %.3g at (%.17g, %.17g): "
                       "ours %.17g, the host's %.17g\n",
                       name, worst, tol, worst_x, worst_y, worst_got, worst_want);
            }
        } else {
            printf("ok   %-10s worst %.3g (claimed %.3g)\n", name, worst, tol);
        }
    }
    fclose(f);

    /* M155. What this library does when it is ASKED something it cannot
       answer, which no sweep over a valid domain reaches.

       <math.h> claims math_errhandling == MATH_ERREXCEPT, which is a promise
       about exactly these cases: a domain error raises FE_INVALID, a pole
       error raises FE_DIVBYZERO, an overflow raises FE_OVERFLOW. The host's
       libm makes the same promise, so it is the oracle here the same way it
       is for every value above - nothing in this table says what the right
       flag is, a library nobody here wrote decides.

       The NaN rows are the half that is easy to get wrong in the other
       direction: a NaN that arrives as an argument is not an error, and a
       library that raises on one is as wrong as a library that stays quiet
       on a real one.

       Every argument goes through a volatile so that neither compiler folds
       the call at compile time. A folded call raises nothing, which would
       make this table pass by not running. */
    int flag_failures = 0;
    int flag_graded = 0;
    int flag_lax = 0;

/* The result goes into a volatile rather than being cast to void, and that
   is the second way this table found to measure nothing. acosh and its
   neighbours are builtins the compiler knows are pure, so a call whose value
   is discarded is dead code and gets deleted - and a call that never happens
   raises no flag. It deleted the ORACLE's call and not this library's,
   because lean_acosh is an ordinary external function it has to assume does
   something. The measurement then said the host's libm does not report a
   domain error in acosh, which is false. */
#define RAISED(expression)                                                    \
    (feclearexcept(FE_ALL_EXCEPT), sink = (expression),                       \
     fetestexcept(FE_ALL_EXCEPT))
#define FLAG_CASE(label, ours, theirs)                                        \
    do {                                                                      \
        int mine = lean_errors(RAISED(ours));                                 \
        int host = host_errors(RAISED(theirs));                               \
        flag_graded++;                                                        \
        if (mine != host) {                                                   \
            printf("FAIL %-18s raised %s, the host's libm raised %s\n",       \
                   label, flag_names(mine), flag_names(host));                \
            flag_failures++;                                                  \
        }                                                                     \
    } while (0)

/* For the cases where the host is NOT the oracle, because it is wrong. The
   standard is, and the expectation is spelled out here with the clause that
   settles it. The host's answer is still measured and still printed, so the
   day it starts reporting one of these the note stops being true and this
   says so rather than quietly agreeing.

   The iconv test learned this first and wrote it down: the host is not
   always one oracle. It is the right one for a value, where the question is
   what a correctly rounded answer looks like and every implementation is
   trying to produce the same number. It is not automatically the right one
   for whether an error was reported, where an implementation is allowed to
   be lax and some are. */
#define FLAG_CASE_STANDARD_SAYS(label, ours, theirs, required, clause)        \
    do {                                                                      \
        int mine = lean_errors(RAISED(ours));                                 \
        int host = host_errors(RAISED(theirs));                               \
        flag_graded++;                                                        \
        if (mine != (required)) {                                             \
            printf("FAIL %-18s raised %s, %s requires %s\n", label,           \
                   flag_names(mine), clause, flag_names(required));           \
            flag_failures++;                                                  \
        } else if (host == mine) {                                            \
            printf("note %-18s the host's libm reports this now too - the "   \
                   "exception below is stale\n", label);                      \
        } else {                                                              \
            flag_lax++;                                                       \
        }                                                                     \
    } while (0)
/* The read is what matters. An assignment to a volatile has the value that
   was assigned, so (v = -1.0) is still the constant -1.0 as far as the
   compiler is concerned and sqrt of it folds at compile time - which raises
   nothing, and would have made this whole table agree by never running.
   Reading v back after the store is what forces the call. */
#define ONE(value) (v = (value), v)
#define TWO(a, b) (v = (a), w = (b))

    volatile double v, w, sink;
    FLAG_CASE("sqrt(-1)", lean_sqrt(ONE(-1.0)), sqrt(ONE(-1.0)));
    FLAG_CASE("sqrt(nan)", lean_sqrt(ONE(NAN)), sqrt(ONE(NAN)));
    FLAG_CASE("log(0)", lean_log(ONE(0.0)), log(ONE(0.0)));
    FLAG_CASE("log(-1)", lean_log(ONE(-1.0)), log(ONE(-1.0)));
    FLAG_CASE("log(nan)", lean_log(ONE(NAN)), log(ONE(NAN)));
    FLAG_CASE("asin(2)", lean_asin(ONE(2.0)), asin(ONE(2.0)));
    FLAG_CASE("asin(nan)", lean_asin(ONE(NAN)), asin(ONE(NAN)));
    FLAG_CASE("acos(2)", lean_acos(ONE(2.0)), acos(ONE(2.0)));
    FLAG_CASE("acosh(0)", lean_acosh(ONE(0.0)), acosh(ONE(0.0)));
    FLAG_CASE("acosh(nan)", lean_acosh(ONE(NAN)), acosh(ONE(NAN)));
    FLAG_CASE("atanh(2)", lean_atanh(ONE(2.0)), atanh(ONE(2.0)));
    FLAG_CASE("atanh(1)", lean_atanh(ONE(1.0)), atanh(ONE(1.0)));
    FLAG_CASE("atanh(-1)", lean_atanh(ONE(-1.0)), atanh(ONE(-1.0)));
    FLAG_CASE("log1p(-2)", lean_log1p(ONE(-2.0)), log1p(ONE(-2.0)));
    FLAG_CASE("log1p(-1)", lean_log1p(ONE(-1.0)), log1p(ONE(-1.0)));
    FLAG_CASE("exp(1000)", lean_exp(ONE(1000.0)), exp(ONE(1000.0)));
    FLAG_CASE("exp(nan)", lean_exp(ONE(NAN)), exp(ONE(NAN)));
    FLAG_CASE("exp2(2000)", lean_exp2(ONE(2000.0)), exp2(ONE(2000.0)));
    FLAG_CASE("sin(inf)", lean_sin(ONE(INFINITY)), sin(ONE(INFINITY)));
    FLAG_CASE("sin(nan)", lean_sin(ONE(NAN)), sin(ONE(NAN)));
    FLAG_CASE("cos(inf)", lean_cos(ONE(INFINITY)), cos(ONE(INFINITY)));
    FLAG_CASE("tan(inf)", lean_tan(ONE(INFINITY)), tan(ONE(INFINITY)));
    FLAG_CASE_STANDARD_SAYS("fmod(1,0)",
              (TWO(1.0, 0.0), lean_fmod(v, w)), (TWO(1.0, 0.0), fmod(v, w)),
              ERROR_INVALID, "C99 F.9.7.1");
    FLAG_CASE_STANDARD_SAYS("fmod(inf,1)",
              (TWO(INFINITY, 1.0), lean_fmod(v, w)),
              (TWO(INFINITY, 1.0), fmod(v, w)),
              ERROR_INVALID, "C99 F.9.7.1");
    FLAG_CASE("fmod(nan,1)",
              (TWO(NAN, 1.0), lean_fmod(v, w)), (TWO(NAN, 1.0), fmod(v, w)));
    FLAG_CASE("pow(0,-1)",
              (TWO(0.0, -1.0), lean_pow(v, w)), (TWO(0.0, -1.0), pow(v, w)));
    FLAG_CASE("pow(-2,0.5)",
              (TWO(-2.0, 0.5), lean_pow(v, w)), (TWO(-2.0, 0.5), pow(v, w)));
    FLAG_CASE("pow(2,3)",
              (TWO(2.0, 3.0), lean_pow(v, w)), (TWO(2.0, 3.0), pow(v, w)));
#undef TWO
#undef ONE
#undef FLAG_CASE
#undef RAISED

#undef FLAG_CASE_STANDARD_SAYS
    printf("ok   %-10s %d error cases raise what the host's libm raises, "
           "%d what C99 requires and it does not (%d disagree)\n",
           "fenv", flag_graded - flag_lax, flag_lax, flag_failures);
    failures += flag_failures;

    /* The two functions no sweep can grade, because what they return is
       not equal to itself. */
    if (!isnan(lean_nan("")) || !isnan(lean_nanf(""))) {
        printf("FAIL %-10s did not return a NaN\n", "nan");
        failures++;
    } else {
        printf("ok   %-10s returns a quiet NaN (isnan, not a tolerance)\n",
               "nan/nanf");
    }

    printf("math-test: %d functions graded against the host's libm, "
           "%d not graded here, %d over their claimed tolerance\n",
           graded, skipped, failures);
    return failures ? 1 : 0;
}
