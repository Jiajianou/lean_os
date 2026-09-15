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
float nextafterf(float x, float y);

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

#ifdef __cplusplus
}
#endif
