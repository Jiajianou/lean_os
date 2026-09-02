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
#define NAN      (__builtin_nanf(""))
#define INFINITY (__builtin_inff())

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

#ifdef __cplusplus
}
#endif
