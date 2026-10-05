#include <fenv.h>
#include <limits.h>
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
double lean_tgamma(double), lean_lgamma(double);
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
   where the exception flags have the x87's own bit layout, and it runs on
   the host, whose flag word has the host's. Getting this wrong is quiet
   rather than loud: the first run of this table reported that log(0) raises
   FE_OVERFLOW, which is this project's FE_DIVBYZERO (0x04) read as the
   host's FE_OVERFLOW (0x04). Both libraries were right and the reader was
   wrong.

   The translation happens where the library RAISES, not where this file
   reads. tools/math-test.sh compiles math.c with feraiseexcept renamed to
   lean_test_feraiseexcept below, which maps this project's bits onto the
   host's and raises those. Reading instead with two tables - which is what
   this file did until the x86_64 host - is only right where the two layouts
   agree, and they agree on an x86_64 Mac and not on an arm64 one: there a
   flag the library's ARITHMETIC raises lands at the host's bit while a flag
   it raises by hand landed at this project's, and 0x04 meant both overflow
   and divide-by-zero in the same word. Now every flag in the word is the
   host's, whichever way it got there, and host_errors reads all of it.

   The values are the ones in user_space/libc/include/fenv.h. They are spelled
   out rather than included because the host's <fenv.h> is already in scope
   here and one translation unit cannot have both. */
#define LEAN_FE_INVALID   0x01
#define LEAN_FE_DIVBYZERO 0x04
#define LEAN_FE_OVERFLOW  0x08
#define LEAN_FE_UNDERFLOW 0x10
#define LEAN_FE_INEXACT   0x20

int lean_test_feraiseexcept(int flags);
int lean_test_feraiseexcept(int flags) {
    return feraiseexcept((flags & LEAN_FE_INVALID ? FE_INVALID : 0) |
                         (flags & LEAN_FE_DIVBYZERO ? FE_DIVBYZERO : 0) |
                         (flags & LEAN_FE_OVERFLOW ? FE_OVERFLOW : 0) |
                         (flags & LEAN_FE_UNDERFLOW ? FE_UNDERFLOW : 0) |
                         (flags & LEAN_FE_INEXACT ? FE_INEXACT : 0));
}

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
float lean_tgammaf(float), lean_lgammaf(float);
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
    {.name = "tgammaf", .oursf = lean_tgammaf, .theirsf = tgammaf},
    {.name = "lgammaf", .oursf = lean_lgammaf, .theirsf = lgammaf},
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
    {.name = "tgamma", .ours1 = lean_tgamma, .theirs1 = tgamma},
    {.name = "lgamma", .ours1 = lean_lgamma, .theirs1 = lgamma},
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

/* Every function at the arguments Annex F has a rule for rather than a
   value: a quiet NaN in each position, both zeros and both infinities -
   and what it RAISED there, against the host's libm, as well as what it
   returned.

   The table in main grades twenty-eight error cases somebody thought of.
   This grades the arguments nobody has to think of, and the reason it
   exists is the one it found first: asin(NaN) raised FE_INVALID here, and
   so did sqrt, fabs, atan and others, because `if (x < 0.0)` is a
   signaling comparison and x86's COMISD signals on a quiet NaN. The sweeps
   never noticed - they compare values, and NaN was the right value. And on
   an arm64 host nothing here could have noticed, because that host's
   compiler emits a quiet compare for the same line. What signals is decided
   by the compiler, so the compiler that ships is graded as well:
   /bin/mathltest asks the same question of x86_64-elf-gcc's output on the
   machine, and that is the [m142q] boot marker.

   An integer result is compared only where the argument is a zero; at a
   NaN or an infinity its value is unspecified and the flag is the whole of
   the answer. */
double lean_frexp(double, int *), lean_ldexp(double, int);
double lean_modf(double, double *), lean_remquo(double, double, int *);
double lean_scalbn(double, int), lean_scalbln(double, long);
double lean_nexttoward(double, long double);
float lean_fmaf(float, float, float), lean_scalblnf(float, long);
float lean_frexpf(float, int *), lean_modff(float, float *);
float lean_remquof(float, float, int *), lean_nexttowardf(float, long double);
int lean_ilogbf(float);

/* The '-' rows' functions, in the shapes TABLE already has. An exponent or
   a quotient that is unspecified at a NaN is simply not returned. */
static double lean_frexp_fraction(double x) { int e; return lean_frexp(x, &e); }
static double host_frexp_fraction(double x) { int e; return frexp(x, &e); }
static double lean_ldexp_3(double x) { return lean_ldexp(x, 3); }
static double host_ldexp_3(double x) { return ldexp(x, 3); }
static double lean_modf_fraction(double x) { double i; return lean_modf(x, &i); }
static double host_modf_fraction(double x) { double i; return modf(x, &i); }
static double lean_modf_whole(double x) { double i; lean_modf(x, &i); return i; }
static double host_modf_whole(double x) { double i; modf(x, &i); return i; }
static double lean_remquo_remainder(double x, double y) { int q; return lean_remquo(x, y, &q); }
static double host_remquo_remainder(double x, double y) { int q; return remquo(x, y, &q); }
static double lean_scalbn_3(double x) { return lean_scalbn(x, 3); }
static double host_scalbn_3(double x) { return scalbn(x, 3); }
static double lean_scalbln_3(double x) { return lean_scalbln(x, 3L); }
static double host_scalbln_3(double x) { return scalbln(x, 3L); }
static double lean_nexttoward_double(double x, double y) { return lean_nexttoward(x, (long double)y); }
static double host_nexttoward_double(double x, double y) { return nexttoward(x, (long double)y); }
static long lean_ilogbf_long(float x) { return lean_ilogbf(x); }
static long host_ilogbf_long(float x) { return ilogbf(x); }
static float lean_scalblnf_3(float x) { return lean_scalblnf(x, 3L); }
static float host_scalblnf_3(float x) { return scalblnf(x, 3L); }
static float lean_frexpf_fraction(float x) { int e; return lean_frexpf(x, &e); }
static float host_frexpf_fraction(float x) { int e; return frexpf(x, &e); }
static float lean_modff_fraction(float x) { float i; return lean_modff(x, &i); }
static float host_modff_fraction(float x) { float i; return modff(x, &i); }
static float lean_modff_whole(float x) { float i; lean_modff(x, &i); return i; }
static float host_modff_whole(float x) { float i; modff(x, &i); return i; }
static float lean_remquof_remainder(float x, float y) { int q; return lean_remquof(x, y, &q); }
static float host_remquof_remainder(float x, float y) { int q; return remquof(x, y, &q); }
static float lean_nexttowardf_float(float x, float y) { return lean_nexttowardf(x, (long double)y); }
static float host_nexttowardf_float(float x, float y) { return nexttowardf(x, (long double)y); }
static float lean_fmaf_entry(float x, float y, float z) { return lean_fmaf(x, y, z); }

static const struct entry UNSWEPT[] = {
    {.name = "frexp", .ours1 = lean_frexp_fraction, .theirs1 = host_frexp_fraction},
    {.name = "ldexp", .ours1 = lean_ldexp_3, .theirs1 = host_ldexp_3},
    {.name = "modf", .ours1 = lean_modf_fraction, .theirs1 = host_modf_fraction},
    {.name = "modf", .ours1 = lean_modf_whole, .theirs1 = host_modf_whole},
    {.name = "remquo", .ours2 = lean_remquo_remainder, .theirs2 = host_remquo_remainder},
    {.name = "scalbn", .ours1 = lean_scalbn_3, .theirs1 = host_scalbn_3},
    {.name = "scalbln", .ours1 = lean_scalbln_3, .theirs1 = host_scalbln_3},
    {.name = "nexttoward", .ours2 = lean_nexttoward_double, .theirs2 = host_nexttoward_double},
    {.name = "fmaf"},
    {.name = "ilogbf", .ourslf = lean_ilogbf_long, .theirslf = host_ilogbf_long},
    {.name = "scalblnf", .oursf = lean_scalblnf_3, .theirsf = host_scalblnf_3},
    {.name = "frexpf", .oursf = lean_frexpf_fraction, .theirsf = host_frexpf_fraction},
    {.name = "modff", .oursf = lean_modff_fraction, .theirsf = host_modff_fraction},
    {.name = "modff", .oursf = lean_modff_whole, .theirsf = host_modff_whole},
    {.name = "remquof", .oursF = lean_remquof_remainder, .theirsF = host_remquof_remainder},
    {.name = "nexttowardf", .oursF = lean_nexttowardf_float, .theirsF = host_nexttowardf_float},
};

/* Where the host is not the oracle. Everywhere else in this sweep the
   host's libm is, and it is a fair one only where the standard has one
   answer AND both Macs this harness runs on give it. Three kinds of edge
   are decided here instead, by the clause that decides them, so that what
   is graded is this library and not the host it happens to run on:

   - a quiet NaN argument, which is what this sweep is for. C11 F.10: it
     returns a NaN and raises nothing - except the integer roundings and
     ilogb, whose result cannot be a NaN and which must raise invalid, and
     the few functions Annex F gives a number instead. /bin/mathltest's
     [m142q] probe holds the compiler that ships to the same rules.
   - an edge where a host is MEASURED to break the standard. fdim(inf, inf)
     is +0 and raises nothing - x > y is false, so 7.12.12.1 performs no
     subtraction to be invalid about. An x86_64 Mac's fdim subtracts first
     and masks after (vsubsd, vcmpltsd, vandpd) and raises invalid; its fdimf
     masks the operands first and does not, and neither does its fdiml.
     What an arm64 Mac's fdim does there cannot be measured from an Intel
     one, and does not have to be: the clause decides, and the host's answer
     is only counted. (Until this rule graded this library rather than the
     host, this library's fdim WAS the host's fdiml - see tools/math-test.sh
     - and once it was this library's, Apple's clang turned its own fdiml
     into the same speculated subtraction; math_long_double.c says how that
     was closed.) fmod's and lgamma's are FLAG_CASE_STANDARD_SAYS's rules
     below.
   - an edge where the standard lets the HOST choose: the value of
     FP_ILOGB0, and whether ilogb raises invalid at a zero or an infinity,
     which C11 7.12.6.5 permits and C23 F.10.3.8 requires. This library does
     what C23 says, and its FP_ILOGB0 is its own <math.h>'s.
   - an edge where the standard lets EVERY implementation choose, and so
     neither the host's answer nor this library's can be the oracle for the
     other: pow(+-0, -inf) is +inf and "may raise" divide-by-zero (C11
     F.10.4.4, and still "may" in C23; IEEE 754-2008 9.2.1 raises
     nothing). This library raises it (math.c's pole_error) and so does an
     x86_64 Mac's pow and powf (measured); an arm64 Mac's is not measured and
     need not be - the rule accepts either, and still requires +inf and
     nothing else. pow is native in math.c, so this is graded on both hosts.
     The other "may"s Annex F has do not reach this sweep: inexact and
     undeserved underflow are not graded here (F.10 leaves both open), and
     fma(inf, 0, NaN)'s optional invalid needs two edges at once, which the
     three-argument sweep below never passes.

   A host that breaks the standard anywhere else fails this sweep, and the
   answer is a rule here with the clause that settles it - not a tolerance. */
#define LEAN_FP_ILOGB0 (-2147483647 - 1)

enum edge_value {
    VALUE_HOST,        /* whatever the host's libm returned */
    VALUE_EXACT,       /* rule.exact, the sign of a zero included */
    VALUE_NAN,
    VALUE_OTHER,       /* the argument that is not a NaN, exactly */
    VALUE_MAGNITUDE,   /* |first|, with either sign */
    VALUE_UNSPECIFIED, /* an integer result the standard does not fix */
};

struct edge_rule {
    const char *clause; /* 0: the host's libm decides */
    int raises;         /* exactly these ERROR_* flags, apart from: */
    enum edge_value value;
    double exact;
    int may_raise;      /* flags the clause lets an implementation add */
};

/* "pow" is pow or powf - every format this sweep has a function in. */
static int named(const char *name, const char *base) {
    size_t n = strlen(base);
    return strncmp(name, base, n) == 0 &&
           (name[n] == '\0' || (name[n] == 'f' && name[n + 1] == '\0'));
}

static int integer_result(const char *name) {
    return named(name, "lrint") || named(name, "llrint") ||
           named(name, "lround") || named(name, "llround") ||
           named(name, "ilogb");
}

static struct edge_rule edge_standard_says(const char *name, double first,
                                           double second, int nan_argument) {
    struct edge_rule rule = {0, 0, VALUE_HOST, 0.0, 0};
    int ilogb = named(name, "ilogb");
    int rounding = integer_result(name) && !ilogb;
    if (nan_argument) {
        if (rounding) {
            return (struct edge_rule){"C11 F.10.6.5 and F.10.6.7",
                                      ERROR_INVALID, VALUE_UNSPECIFIED, 0.0, 0};
        }
        if (ilogb) {
            return (struct edge_rule){"C23 F.10.3.8", ERROR_INVALID,
                                      VALUE_UNSPECIFIED, 0.0, 0};
        }
        if (named(name, "pow") && (second == 0.0 || first == 1.0)) {
            return (struct edge_rule){"C11 F.10.4.4", 0, VALUE_EXACT, 1.0, 0};
        }
        if (named(name, "hypot") && (isinf(first) || isinf(second))) {
            return (struct edge_rule){"C11 F.10.4.3", 0, VALUE_EXACT,
                                      INFINITY, 0};
        }
        if ((named(name, "fmax") || named(name, "fmin")) &&
            isnan(first) != isnan(second)) {
            return (struct edge_rule){"C11 F.10.9.2", 0, VALUE_OTHER, 0.0, 0};
        }
        /* copysign is IEC 60559's copySign, a quiet bit operation; which
           sign a NaN carries is not something the standard fixes. */
        if (named(name, "copysign") && !isnan(first)) {
            return (struct edge_rule){"C11 F.3", 0, VALUE_MAGNITUDE, 0.0, 0};
        }
        return (struct edge_rule){"C11 F.10, a NaN argument", 0, VALUE_NAN,
                                  0.0, 0};
    }
    if (named(name, "fmod") && (isinf(first) || second == 0.0)) {
        return (struct edge_rule){"C11 F.10.7.1", ERROR_INVALID, VALUE_NAN,
                                  0.0, 0};
    }
    if (named(name, "lgamma") && first == 0.0) {
        return (struct edge_rule){"C11 F.10.5.3", ERROR_DIVBYZERO, VALUE_EXACT,
                                  INFINITY, 0};
    }
    if (named(name, "pow") && first == 0.0 && isinf(second) &&
        signbit(second)) {
        return (struct edge_rule){"C11 F.10.4.4", 0, VALUE_EXACT, INFINITY,
                                  ERROR_DIVBYZERO};
    }
    if (named(name, "fdim") && isinf(first) && first == second) {
        return (struct edge_rule){"C11 7.12.12.1", 0, VALUE_EXACT, 0.0, 0};
    }
    if (ilogb && first == 0.0) {
        return (struct edge_rule){"C23 F.10.3.8", ERROR_INVALID, VALUE_EXACT,
                                  (double)LEAN_FP_ILOGB0, 0};
    }
    if (ilogb && isinf(first)) {
        return (struct edge_rule){"C23 F.10.3.8", ERROR_INVALID, VALUE_EXACT,
                                  (double)INT_MAX, 0};
    }
    if (rounding && isinf(first)) {
        return (struct edge_rule){"C11 F.10.6.5 and F.10.6.7", ERROR_INVALID,
                                  VALUE_UNSPECIFIED, 0.0, 0};
    }
    return rule;
}

/* Every call either graded, or not this library's to grade on this host -
   see tests/math/host_long_double.c. */
static int edge_calls, edge_failures, edge_decided, edge_host_differs;
static int edge_reached_host;
struct name_set {
    const char *names[256];
    int count;
};

static void name_set_add(struct name_set *set, const char *name) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->names[i], name) == 0) {
            return;
        }
    }
    if (set->count < (int)(sizeof(set->names) / sizeof(set->names[0]))) {
        set->names[set->count++] = name;
    }
}

static int name_set_has(const struct name_set *set, const char *name) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void name_set_print(const struct name_set *set) {
    for (int i = 0; i < set->count; i++) {
        printf("%s%s", i ? " " : "", set->names[i]);
    }
}

/* Which functions met a NaN here, graded as this library's - and which met
   one only through the host's long double libm, on a host where this
   library's long double half cannot run. */
static struct name_set edge_probed, edge_probed_elsewhere;

/* tests/math/host_long_double.c calls this from every function it stands
   in for, so that an answer which passed through the host's long double
   libm is known to have. On an x86_64 host nothing calls it: this library's
   own math_long_double.c is linked instead. */
static unsigned long host_long_double_calls;
static struct name_set host_long_double_names;
void lean_test_host_long_double_reached(const char *name);
void lean_test_host_long_double_reached(const char *name) {
    host_long_double_calls++;
    name_set_add(&host_long_double_names, name);
}

static const char *edge_show(double x, char *buffer, size_t size) {
    if (isnan(x)) {
        return "nan";
    }
    if (isinf(x)) {
        return signbit(x) ? "-inf" : "+inf";
    }
    if (x == 0.0) {
        return signbit(x) ? "-0" : "+0";
    }
    snprintf(buffer, size, "%g", x);
    return buffer;
}

static int a_zero_pair(const char *name, double first, double second) {
    return (named(name, "fmax") || named(name, "fmin")) &&
           first == 0.0 && second == 0.0;
}

/* Whether a result is the one the rule names. A zero's sign counts unless
   the rule says it does not. */
static int value_is(const struct edge_rule *rule, double got, double first,
                    double second, double tolerance) {
    double want;
    switch (rule->value) {
    case VALUE_NAN:
        return isnan(got);
    case VALUE_UNSPECIFIED:
        return 1;
    case VALUE_MAGNITUDE:
        return fabs(got) == fabs(first);
    case VALUE_OTHER:
        want = isnan(first) ? second : first;
        return got == want && signbit(got) == signbit(want);
    case VALUE_EXACT:
        want = rule->exact;
        break;
    default:
        return 0;
    }
    if (error_of(got, want) > tolerance) {
        return 0;
    }
    return !(got == 0.0 && want == 0.0 && signbit(got) != signbit(want));
}

static const char *value_named(const struct edge_rule *rule, double first,
                               double second, char *buffer, size_t size) {
    switch (rule->value) {
    case VALUE_NAN:
        return "a NaN";
    case VALUE_MAGNITUDE:
        snprintf(buffer, size, "+-%g", fabs(first));
        return buffer;
    case VALUE_OTHER:
        return edge_show(isnan(first) ? second : first, buffer, size);
    default:
        return edge_show(rule->exact, buffer, size);
    }
}

static void edge_check(const char *name, double first, double second,
                       int nan_argument, int reached_host, const char *label,
                       double got, int got_raised, double want,
                       int want_raised, double tolerance, int value_graded) {
    edge_calls++;
    /* This answer passed through the host's long double libm, which stood in
       for this library's on a host that cannot run it - so it says nothing
       about this library, at an edge the standard decides or anywhere else.
       /bin/mathltest asks the same question of this library on the target. */
    if (reached_host) {
        edge_reached_host++;
        if (nan_argument) {
            name_set_add(&edge_probed_elsewhere, name);
        }
        return;
    }
    if (nan_argument) {
        name_set_add(&edge_probed, name);
    }
    int mine = host_errors(got_raised);
    int host = host_errors(want_raised);
    struct edge_rule rule = edge_standard_says(name, first, second, nan_argument);
    if (rule.clause) {
        /* The standard is the oracle, so the host's answer is only counted -
           it is never what this library is compared with. */
        edge_decided++;
        if ((host & ~rule.may_raise) != rule.raises ||
            !value_is(&rule, want, first, second, tolerance)) {
            edge_host_differs++;
        }
        if ((mine & ~rule.may_raise) != rule.raises) {
            printf("FAIL %-22s raised %s, %s requires %s", label,
                   flag_names(mine), rule.clause, flag_names(rule.raises));
            if (rule.may_raise) {
                printf(" (and permits %s)", flag_names(rule.may_raise));
            }
            printf("\n");
            edge_failures++;
        }
        if (!value_is(&rule, got, first, second, tolerance)) {
            char a[32], b[32];
            printf("FAIL %-22s returned %s (%.17g), %s requires %s\n", label,
                   edge_show(got, a, sizeof(a)), got, rule.clause,
                   value_named(&rule, first, second, b, sizeof(b)));
            edge_failures++;
        }
        return;
    }
    if (mine != host) {
        printf("FAIL %-22s raised %s, the host's libm raised %s\n", label,
               flag_names(mine), flag_names(host));
        edge_failures++;
    }
    /* Which zero fmax(-0, +0) returns is the one sign Annex F leaves open,
       and says so in a footnote to F.10.9.2. This library returns +0 from
       fmax and -0 from fmin; the host returns its first argument. */
    int zero_sign_open = a_zero_pair(name, first, second);
    if (value_graded) {
        int wrong = error_of(got, want) > tolerance;
        if (!wrong && got == 0.0 && want == 0.0 && !zero_sign_open &&
            signbit(got) != signbit(want)) {
            wrong = 1;
        }
        if (wrong) {
            char a[32], b[32];
            printf("FAIL %-22s returned %s (%.17g), the host's %s (%.17g)\n",
                   label, edge_show(got, a, sizeof(a)), got,
                   edge_show(want, b, sizeof(b)), want);
            edge_failures++;
        }
    }
}
/* Volatile in and out, for the two reasons the error table below gives at
   length: a call on a constant folds and raises nothing, and a call whose
   result is unused is deleted. */
#define CAPTURE(result, raised, expression)                                   \
    do {                                                                      \
        feclearexcept(FE_ALL_EXCEPT);                                         \
        result = (expression);                                                \
        raised = fetestexcept(FE_ALL_EXCEPT);                                 \
    } while (0)

static const double EDGES[] = {NAN, 0.0, -0.0, INFINITY, -INFINITY};
#define EDGE_COUNT ((int)(sizeof(EDGES) / sizeof(EDGES[0])))
/* The edges and two ordinary values, so a two-argument function meets a
   NaN beside a number as well as beside another edge. */
static const double PARTNERS[] = {NAN, 0.0, -0.0, INFINITY, -INFINITY, 1.0, -2.5};
#define PARTNER_COUNT ((int)(sizeof(PARTNERS) / sizeof(PARTNERS[0])))

static void edge_sweep_one(const struct entry *e) {
    volatile double a, b, c, got, want;
    volatile long long got_integer, want_integer;
    int got_raised, want_raised;
    char label[96], x[32], y[32], z[32];

    for (int i = 0; i < EDGE_COUNT; i++) {
        a = EDGES[i];
        snprintf(label, sizeof(label), "%s(%s)", e->name, edge_show(a, x, sizeof(x)));
        int integer = 0;
        unsigned long before = host_long_double_calls;
        if (e->ours1) {
            CAPTURE(got, got_raised, e->ours1(a));
            CAPTURE(want, want_raised, e->theirs1(a));
        } else if (e->oursf) {
            CAPTURE(got, got_raised, (double)e->oursf((float)a));
            CAPTURE(want, want_raised, (double)e->theirsf((float)a));
        } else if (e->oursfi) {
            snprintf(label, sizeof(label), "%s(%s, 3)", e->name, edge_show(a, x, sizeof(x)));
            CAPTURE(got, got_raised, (double)e->oursfi((float)a, 3));
            CAPTURE(want, want_raised, (double)e->theirsfi((float)a, 3));
        } else if (e->oursl) {
            integer = 1;
            CAPTURE(got_integer, got_raised, e->oursl(a));
            CAPTURE(want_integer, want_raised, e->theirsl(a));
        } else if (e->ourslf) {
            integer = 1;
            CAPTURE(got_integer, got_raised, e->ourslf((float)a));
            CAPTURE(want_integer, want_raised, e->theirslf((float)a));
        } else if (e->oursll) {
            integer = 1;
            CAPTURE(got_integer, got_raised, e->oursll(a));
            CAPTURE(want_integer, want_raised, e->theirsll(a));
        } else if (e->oursllf) {
            integer = 1;
            CAPTURE(got_integer, got_raised, e->oursllf((float)a));
            CAPTURE(want_integer, want_raised, e->theirsllf((float)a));
        } else {
            break;
        }
        if (integer) {
            got = (double)got_integer;
            want = (double)want_integer;
        }
        double tolerance = (e->ours1 || integer) ? 1e-12 : 1.2e-7;
        edge_check(e->name, EDGES[i], 1.0, isnan(EDGES[i]),
                   host_long_double_calls != before, label, got, got_raised,
                   want, want_raised, tolerance, !integer || EDGES[i] == 0.0);
    }

    if (e->ours2 || e->oursF) {
        for (int i = 0; i < PARTNER_COUNT; i++) {
            for (int j = 0; j < PARTNER_COUNT; j++) {
                if (i >= EDGE_COUNT && j >= EDGE_COUNT) {
                    continue;
                }
                a = PARTNERS[i];
                b = PARTNERS[j];
                snprintf(label, sizeof(label), "%s(%s, %s)", e->name,
                         edge_show(a, x, sizeof(x)), edge_show(b, y, sizeof(y)));
                unsigned long before = host_long_double_calls;
                if (e->ours2) {
                    CAPTURE(got, got_raised, e->ours2(a, b));
                    CAPTURE(want, want_raised, e->theirs2(a, b));
                } else {
                    CAPTURE(got, got_raised, (double)e->oursF((float)a, (float)b));
                    CAPTURE(want, want_raised, (double)e->theirsF((float)a, (float)b));
                }
                edge_check(e->name, PARTNERS[i], PARTNERS[j],
                           isnan(PARTNERS[i]) || isnan(PARTNERS[j]),
                           host_long_double_calls != before, label, got,
                           got_raised, want, want_raised,
                           e->ours2 ? 1e-12 : 1.2e-7, 1);
            }
        }
    }

    int fused_float = strcmp(e->name, "fmaf") == 0;
    if (e->ours3 || fused_float) {
        /* One edge at a time with the other two ordinary: fma(0, inf, NaN)
           may raise or not (F.10.10.1), and this grades what the standard
           decides. */
        for (int position = 0; position < 3; position++) {
            for (int i = 0; i < EDGE_COUNT; i++) {
                double arguments[3] = {1.5, -2.5, 0.75};
                arguments[position] = EDGES[i];
                a = arguments[0];
                b = arguments[1];
                c = arguments[2];
                snprintf(label, sizeof(label), "%s(%s, %s, %s)", e->name,
                         edge_show(a, x, sizeof(x)), edge_show(b, y, sizeof(y)),
                         edge_show(c, z, sizeof(z)));
                unsigned long before = host_long_double_calls;
                if (e->ours3) {
                    CAPTURE(got, got_raised, e->ours3(a, b, c));
                    CAPTURE(want, want_raised, e->theirs3(a, b, c));
                } else {
                    CAPTURE(got, got_raised,
                            (double)lean_fmaf_entry((float)a, (float)b, (float)c));
                    CAPTURE(want, want_raised,
                            (double)fmaf((float)a, (float)b, (float)c));
                }
                edge_check(e->name, arguments[0], arguments[1],
                           isnan(EDGES[i]), host_long_double_calls != before,
                           label, got, got_raised, want, want_raised,
                           e->ours3 ? 1e-12 : 1.2e-7, 1);
            }
        }
    }
}
#undef CAPTURE

static int edge_sweep(char row_names[][64], int row_count) {
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        edge_sweep_one(&TABLE[i]);
    }
    for (size_t i = 0; i < sizeof(UNSWEPT) / sizeof(UNSWEPT[0]); i++) {
        edge_sweep_one(&UNSWEPT[i]);
    }
    /* Coverage: every function with a row in cases.tsv met a NaN above, or
       this names the one that did not. nan and nanf take a string. A
       function that met one only through the host's long double libm is
       covered - on the target, by /bin/mathltest - and is named below. */
    int unprobed = 0;
    for (int i = 0; i < row_count; i++) {
        if (strcmp(row_names[i], "nan") == 0 || strcmp(row_names[i], "nanf") == 0) {
            continue;
        }
        if (!name_set_has(&edge_probed, row_names[i]) &&
            !name_set_has(&edge_probed_elsewhere, row_names[i])) {
            printf("FAIL %-22s has a row in cases.tsv and no NaN reached it - "
                   "add it to TABLE or UNSWEPT in tests/math/main.c\n",
                   row_names[i]);
            unprobed++;
        }
    }
    if (edge_failures == 0 && unprobed == 0) {
        printf("ok   %-10s %d calls at a quiet NaN in every position, +-0 "
               "and +-inf: %d graded by the clause of C11 or C23 that decides "
               "them (the host's libm disagrees at %d), %d against the host's "
               "libm, and all %d functions with a row met a NaN\n", "edges",
               edge_calls, edge_decided, edge_host_differs,
               edge_calls - edge_decided - edge_reached_host, row_count - 2);
    }
    if (edge_reached_host != 0) {
        printf("note %-10s %d calls reached the host's long double libm in "
               "place of this library's and were NOT graded here - "
               "math_long_double.c is x87 code and was not linked into this "
               "run. /bin/mathltest grades those on the target. Met a NaN "
               "only that way: ", "edges", edge_reached_host);
        name_set_print(&edge_probed_elsewhere);
        printf("\n");
    }
    return edge_failures + unprobed;
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
    static char row_names[256][64];
    int row_count = 0;

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
        if (row_count < (int)(sizeof(row_names) / sizeof(row_names[0]))) {
            snprintf(row_names[row_count++], sizeof(row_names[0]), "%s", name);
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
        int mine = host_errors(RAISED(ours));                                 \
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
        int mine = host_errors(RAISED(ours));                                 \
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

    failures += edge_sweep(row_names, row_count);

    /* The two functions no sweep can grade, because what they return is
       not equal to itself. */
    if (!isnan(lean_nan("")) || !isnan(lean_nanf(""))) {
        printf("FAIL %-10s did not return a NaN\n", "nan");
        failures++;
    } else {
        printf("ok   %-10s returns a quiet NaN (isnan, not a tolerance)\n",
               "nan/nanf");
    }

    /* The XSI constants. They are not functions, so the sweep above cannot
       see them, and they failed to exist for ninety-nine milestones without
       anything noticing - until pffft, reached through WebRTC in Chromium's
       //cc graph, asked for M_SQRT2 in a build that had already compiled a
       long double libm. The oracle is the host's libm rather than the host's
       own M_ macros: comparing two headers' decimal literals would only say
       that somebody copied them correctly, and what is worth knowing is
       whether the number IS the number. */
    {
        extern const double lean_math_constants[];
        extern const char *const lean_math_constant_names[];
        extern const int lean_math_constant_count;

        const double pi = acos(-1.0);
        const double oracle[] = {
            exp(1.0), 1.0 / log(2.0), 1.0 / log(10.0), log(2.0), log(10.0),
            pi, pi / 2.0, pi / 4.0, 1.0 / pi, 2.0 / pi,
            2.0 / sqrt(pi), sqrt(2.0), sqrt(0.5),
        };
        const int oracle_count = (int)(sizeof(oracle) / sizeof(oracle[0]));

        int constant_failures = 0;
        if (oracle_count != lean_math_constant_count) {
            printf("FAIL %-10s this libc declares %d of them and this test "
                   "has %d oracles\n", "M_*",
                   lean_math_constant_count, oracle_count);
            constant_failures++;
        } else {
            for (int i = 0; i < oracle_count; i++) {
                /* Two units in the last place. A decimal literal that rounds
                   to the right double is exact; the derived ones - 2/sqrt(pi)
                   above all - can sit one ulp away from a correctly written
                   constant, and a mistyped digit that mattered at all would
                   be further out than this. */
                double ours = lean_math_constants[i];
                double theirs = oracle[i];
                double tolerance = 2.0 * (nextafter(theirs, INFINITY) - theirs);
                if (!(fabs(ours - theirs) <= tolerance)) {
                    printf("FAIL %-10s ours %.17g, the host's libm says "
                           "%.17g\n", lean_math_constant_names[i], ours,
                           theirs);
                    constant_failures++;
                }
            }
            if (constant_failures == 0) {
                printf("ok   %-10s all %d XSI constants are within two ulps "
                       "of what the host's libm computes\n", "M_*",
                       oracle_count);
            }
        }
        failures += constant_failures;
    }

    /* The sweeps and the specials above cannot skip what the edge sweep
       skips, and on a host where tests/math/host_long_double.c stands in
       for this library's long double half they compared every function
       built on it against the host's - the host's libm against itself. That
       is said here rather than counted as grading. */
    if (host_long_double_calls != 0) {
        printf("note %-10s this run linked tests/math/host_long_double.c, not "
               "math_long_double.c, and %lu calls passed through the host's ",
               "long dbl", host_long_double_calls);
        name_set_print(&host_long_double_names);
        printf(" - the functions math.c builds on those were graded as the "
               "host's here; /bin/mathltest grades this library's on the "
               "target\n");
    }
    printf("math-test: %d functions graded against the host's libm, "
           "%d not graded here, %d over their claimed tolerance\n",
           graded, skipped, failures);
    return failures ? 1 : 0;
}
