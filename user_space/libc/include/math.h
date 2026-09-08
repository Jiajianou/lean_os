/* user_space/libc/include/math.h - M63.
 *
 * The functions the target actually calls, which is the rule M63 set for
 * this whole library: guessing at "a standard library" produces a large
 * pile of functions nothing calls, and porting one real thing produces
 * exactly the ones that matter. These are Whetstone's list (sin, cos,
 * atan, log, exp, sqrt) plus the handful that come free once those
 * exist.
 *
 * Accuracy is stated rather than assumed: these are double-precision
 * minimax/Taylor approximations with proper range reduction, good to
 * around 1e-12 relative over their useful ranges - fine for a benchmark
 * and for graphics, not a replacement for a correctly-rounded libm. The
 * boot self-test checks them against known values so "around 1e-12" is a
 * claim this project can fail rather than one it merely makes.
 */
#pragma once

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_E    2.71828182845904523536

#define HUGE_VAL (__builtin_huge_val())
/* M100: C99's evaluation-width types, which the same libstdc++ probe
 * that wants the comparison macros also names. On x86-64 every
 * expression is evaluated at its own width (FLT_EVAL_METHOD 0), so
 * these are exactly their namesakes. */
typedef float float_t;
typedef double double_t;
#define NAN      (__builtin_nanf(""))
#define INFINITY (__builtin_inff())

/* ---- M100: the float variants harfbuzz named ---------------------------
 *
 * C99 has had `floorf` and its siblings since 1999; nothing ported here
 * had used one until harfbuzz's hb-algs.hh called six of them by name
 * and stopped (and hb-ot-font.cc a seventh, once those six let it
 * compile that far, then three more). These ten, and only these - M63's rule - each
 * the double function rounded once to float, which for floor, ceil and
 * fabs is exact and for the others is within one float ulp of the
 * correctly rounded answer. tools/math-test.sh grades them against the
 * host's own floats like everything else in this header. */
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float hypotf(float x, float y); /* the seventh, from hb-ot-font.cc, once the first six let it compile that far */
float sqrtf(float x);           /* and three more from the pass after that: */
float atanf(float x);
float roundf(float x);          /* named fifty times in harfbuzz, behind a fallback of its own */

double fabs(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);

/* ---- M80 groundwork --------------------------------------------------
 *
 * The classification macros and the decomposition functions, which are
 * what a program doing arithmetic on doubles actually reaches for and
 * which this header did not have. The macros are the compiler's own
 * builtins rather than hand-written bit tests: GCC knows this target's
 * double layout and generates the right comparison, and a bit test
 * written here would be a second opinion about it.
 */
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)
#define isnormal(x)   __builtin_isnormal(x)
/* M100: the six comparison macros, which are the other half of C99's
 * classification set and the half libstdc++'s configure insists on
 * before it will believe <math.h> is C99 at all - its probe names all
 * twelve in one test program, and without these six it concluded this
 * libc had none, so <cmath> defined no std::isnan and harfbuzz's
 * `std::isnan(x)` expanded to `std::__builtin_isnan(x)`. Quiet on NaN,
 * which is the point of them: `isgreater(NAN, 1)` is false without a
 * floating-point exception, where `NAN > 1` is allowed to raise one. */
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *ipart);
double trunc(double x);
double round(double x);
double copysign(double x, double y);
double hypot(double x, double y);
double log2(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double fmax(double a, double b);
double fmin(double a, double b);

/* ---- M99: the C99 set, and why these seven arrived together ----------
 *
 * CPython's configure checks `acosh asinh atanh erf erfc expm1 log1p
 * log2` in one loop and stops with "Python requires C99 compatible
 * libm" on the first one missing. log2 was already here; the rest are
 * the ones whose naive expressions cancel - see math.c for the range
 * each is written around. nextafter comes with them because
 * math.nextafter and math.ulp call it unguarded.
 *
 * Accuracy: graded against the host's own libm by tools/math-test.sh
 * rather than against a table written here, with the tolerance stated
 * per function in tests/math/cases.tsv. */
double expm1(double x);
double log1p(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double erf(double x);
double erfc(double x);
double nextafter(double x, double y);

/* And three from CPython's own math module rather than from a standard's
 * list - see math.c, and fma in particular, which is written out because
 * this target has no FMA instruction and __builtin_fma without one calls
 * fma. */
double cbrt(double x);
double exp2(double x);
double fma(double x, double y, double z);

#ifdef __cplusplus
}
#endif
