#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_E    2.71828182845904523536

#define HUGE_VAL (__builtin_huge_val())
typedef float float_t;
typedef double double_t;
#define NAN      (__builtin_nanf(""))
#define INFINITY (__builtin_inff())

float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float hypotf(float x, float y);
float sqrtf(float x);
float atanf(float x);
float roundf(float x);
float expf(float x);
float tanhf(float x);
float nextafterf(float x, float y);
float atan2f(float y, float x);
float ldexpf(float x, int exponent);

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

#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)
#define isnormal(x)   __builtin_isnormal(x)
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

#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)

#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())

/* How this library reports a domain error, a pole error and an overflow.
   C99 7.12 allows errno, the floating point exception flags, or both, and
   requires the answer to be one of those three - which is why there is no
   fourth spelling for "it does not say".

   This one raises the flags and does not touch errno. That is a decision
   rather than an omission: every function below is built out of x86-64
   instructions whose results the hardware already records in the x87 status
   word and in MXCSR, which <fenv.h> reads - so the flags are a fact about
   what happened either way, and errno would be a second bookkeeping system
   kept by hand beside one the machine keeps for free. The error paths that
   return before any arithmetic runs - a negative argument to sqrt, a zero to
   log - call feraiseexcept themselves, because on those the hardware never
   got to see the problem.

   M155 added these, and V8 is what asked for them: llvm-libc's own ieee754
   reads math_errhandling to decide whether to report at all. They are graded
   by tools/math-test.sh against the host's libm, which is a library nobody
   here wrote answering the same questions. */
#define MATH_ERRNO       1
#define MATH_ERREXCEPT   2
#define math_errhandling MATH_ERREXCEPT

double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *ipart);
double trunc(double x);
double round(double x);
long lround(double x);
long lroundf(float x);
long long llround(double x);
long long llroundf(float x);
double copysign(double x, double y);
double hypot(double x, double y);
double log2(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double nan(const char *tag);
float nanf(const char *tag);
double fmax(double a, double b);
double fmin(double a, double b);

double expm1(double x);
double log1p(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double erf(double x);
double erfc(double x);
double nextafter(double x, double y);

double cbrt(double x);
double exp2(double x);
double fma(double x, double y, double z);

/* M142. long double on this target is the x87 80-bit extended format, which
   is a different type from double rather than a spelling of it, and
   user_space/libc/src/math_long_double.c is x87 instructions for that
   reason. erfl, erfcl, lgammal, tgammal, fmal and strtold are deliberately
   absent: each needs an algorithm at 64-bit mantissa that this project has
   not written, and a (long double) cast of the double one would be a
   precision claim the compiler would let through. The condition for
   building them is a program on this machine that calls one. */
long double fabsl(long double x);
long double copysignl(long double x, long double y);
long double sqrtl(long double x);
long double truncl(long double x);
long double floorl(long double x);
long double ceill(long double x);
long double roundl(long double x);
long double rintl(long double x);
long double nearbyintl(long double x);
long lrintl(long double x);
long long llrintl(long double x);
long lroundl(long double x);
long long llroundl(long double x);
long double frexpl(long double x, int *exponent);
long double ldexpl(long double x, int exponent);
long double scalbnl(long double x, int exponent);
long double scalblnl(long double x, long exponent);
int ilogbl(long double x);
long double logbl(long double x);
long double modfl(long double x, long double *integer_part);
long double expl(long double x);
long double exp2l(long double x);
long double expm1l(long double x);
long double logl(long double x);
long double log2l(long double x);
long double log10l(long double x);
long double log1pl(long double x);
long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double asinl(long double x);
long double acosl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double sinhl(long double x);
long double coshl(long double x);
long double tanhl(long double x);
long double asinhl(long double x);
long double acoshl(long double x);
long double atanhl(long double x);
long double hypotl(long double x, long double y);
long double cbrtl(long double x);
long double powl(long double x, long double y);
long double fmodl(long double x, long double y);
long double remainderl(long double x, long double y);
long double remquol(long double x, long double y, int *quotient);
long double nextafterl(long double x, long double y);
long double nexttowardl(long double x, long double y);
long double fdiml(long double x, long double y);
long double fmaxl(long double x, long double y);
long double fminl(long double x, long double y);
long double nanl(const char *tag);

/* M155. The rest of C99 7.12 - see the note at the top of math.c for why the
   double ones delegate to the long double family and why lgamma and tgamma
   are not among them. V8 is what asked: truncf and nearbyintf first, and then
   the thirty-seven others a float family is. */
double nearbyint(double x);
double rint(double x);
long lrint(double x);
long long llrint(double x);
double remainder(double x, double y);
double remquo(double x, double y, int *quotient);
double logb(double x);
int ilogb(double x);
double scalbn(double x, int exponent);
double scalbln(double x, long exponent);
double fdim(double x, double y);
double nexttoward(double x, long double y);

float acosf(float x);
float asinf(float x);
float acoshf(float x);
float asinhf(float x);
float atanhf(float x);
float coshf(float x);
float sinhf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log10f(float x);
float log1pf(float x);
float log2f(float x);
float logbf(float x);
float cbrtf(float x);
float erff(float x);
float erfcf(float x);
float nearbyintf(float x);
float rintf(float x);
float truncf(float x);
float powf(float x, float y);
float fmodf(float x, float y);
float remainderf(float x, float y);
float copysignf(float x, float y);
float fdimf(float x, float y);
float fmaxf(float x, float y);
float fminf(float x, float y);
float fmaf(float x, float y, float z);
int ilogbf(float x);
long lrintf(float x);
long long llrintf(float x);
float scalbnf(float x, int exponent);
float scalblnf(float x, long exponent);
float frexpf(float x, int *exponent);
float modff(float x, float *ipart);
float remquof(float x, float y, int *quotient);
float nexttowardf(float x, long double y);

#ifdef __cplusplus
}
#endif
