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
