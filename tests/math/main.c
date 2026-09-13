#include <math.h>
#include <stdio.h>
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
long lean_lround(double), lean_lroundf(float);
long long lean_llround(double), lean_llroundf(float);

typedef double (*fn1)(double);
typedef double (*fn2)(double, double);
typedef double (*fn3)(double, double, double);
typedef float (*fn1f)(float);
typedef float (*fn2f)(float, float);
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
    fnl oursl, theirsl;
    fnlf ourslf, theirslf;
    fnll oursll, theirsll;
    fnllf oursllf, theirsllf;
};

static const struct entry TABLE[] = {
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

static double err_of(double got, double want) {
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
            const char *fn; const char *at; const char *why;
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
                if (err_of(got, want) <= special_tol) {
                    continue;
                }
                const char *why = 0;
                for (size_t d = 0; d < sizeof(DIVERGE) / sizeof(DIVERGE[0]); d++) {
                    if (strcmp(DIVERGE[d].fn, TABLE[i].name) == 0 &&
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
                double err = err_of(got, want);
                if (err > worst) {
                    worst = err; worst_x = (double)x; worst_got = got; worst_want = want;
                }
            }
        } else if (kind[0] == 'F') {
            for (int i = 0; i < n; i++) {
                float x = (float)(lo + step * (double)i);
                for (int j = 0; j < n; j++) {
                    float y = (float)(lo + step * (double)j);
                    double got = (double)e->oursF(x, y), want = (double)e->theirsF(x, y);
                    double err = err_of(got, want);
                    if (err > worst) {
                        worst = err; worst_x = (double)x; worst_y = (double)y;
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
                double err = err_of(got, want);
                if (err > worst) {
                    worst = err; worst_x = x; worst_got = got; worst_want = want;
                }
            }
        } else if (kind[0] == '2') {
            for (int i = 0; i < n; i++) {
                double x = lo + step * (double)i;
                for (int j = 0; j < n; j++) {
                    double y = lo + step * (double)j;
                    double got = e->ours2(x, y), want = e->theirs2(x, y);
                    double err = err_of(got, want);
                    if (err > worst) {
                        worst = err; worst_x = x; worst_y = y;
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
                        double err = err_of(got, want);
                        if (err > worst) {
                            worst = err; worst_x = x; worst_y = y;
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

    printf("math-test: %d functions graded against the host's libm, "
           "%d not graded here, %d over their claimed tolerance\n",
           graded, skipped, failures);
    return failures ? 1 : 0;
}
