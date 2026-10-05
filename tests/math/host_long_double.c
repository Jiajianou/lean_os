/* The long double half of the library, on a host where it cannot be.

   math.c calls the long double family: fdim is fdiml cast down, and the same
   is true of rint, nearbyint, lrint, llrint, remainder, remquo, logb, ilogb,
   scalbn and scalbln, while lgamma and tgamma compute in logl, expl, sinl and
   powl. tools/math-test.sh renames every one of those calls to lean_<name>l.
   On an x86_64 host this library's own math_long_double.c answers them - it
   is x87 code, and an x86_64 Mac has an x87. On an arm64 Mac it cannot: there
   long double is double and the file refuses to compile. This file answers
   in its place, with the host's <name>l, and COUNTS every call - so main.c
   can tell an answer this library computed from one the host's libm did, and
   grade only the first as this library's. Whatever is answered here is
   graded on the target instead, by /bin/mathltest, which runs on both hosts.

   Every function user_space/libc/include/math.h declares in the long double
   family is here, whether or not math.c calls it today; a new one that
   math.c calls and this file lacks is an undefined symbol at the link, which
   tools/cross-arch-test.sh builds for the other Mac.

   Whether it answers is decided by the compile's target, as the complement
   of tests/math/long_double_library.c - see that file for why the choice is
   made here and not in the script. */
#if defined(__x86_64__) && __LDBL_MANT_DIG__ == 64 &&                         \
    !defined(LEAN_MATH_TEST_LONG_DOUBLE_HOST)
typedef int lean_math_test_long_double_is_this_library;
#else
#include <math.h>

void lean_test_host_long_double_reached(const char *name);

#define FORWARD(type, name, parameters, arguments)                            \
    type lean_##name parameters;                                              \
    type lean_##name parameters {                                             \
        lean_test_host_long_double_reached(#name);                            \
        return name arguments;                                                \
    }
#define ONE(name) FORWARD(long double, name, (long double x), (x))
#define TWO(name) FORWARD(long double, name, (long double x, long double y), (x, y))

ONE(fabsl)
ONE(sqrtl)
ONE(truncl)
ONE(floorl)
ONE(ceill)
ONE(roundl)
ONE(rintl)
ONE(nearbyintl)
ONE(logbl)
ONE(expl)
ONE(exp2l)
ONE(expm1l)
ONE(logl)
ONE(log2l)
ONE(log10l)
ONE(log1pl)
ONE(sinl)
ONE(cosl)
ONE(tanl)
ONE(asinl)
ONE(acosl)
ONE(atanl)
ONE(sinhl)
ONE(coshl)
ONE(tanhl)
ONE(asinhl)
ONE(acoshl)
ONE(atanhl)
ONE(cbrtl)
TWO(copysignl)
TWO(atan2l)
TWO(hypotl)
TWO(powl)
TWO(fmodl)
TWO(remainderl)
TWO(nextafterl)
TWO(nexttowardl)
TWO(fdiml)
TWO(fmaxl)
TWO(fminl)
FORWARD(long, lrintl, (long double x), (x))
FORWARD(long long, llrintl, (long double x), (x))
FORWARD(long, lroundl, (long double x), (x))
FORWARD(long long, llroundl, (long double x), (x))
FORWARD(int, ilogbl, (long double x), (x))
FORWARD(long double, frexpl, (long double x, int *exponent), (x, exponent))
FORWARD(long double, ldexpl, (long double x, int exponent), (x, exponent))
FORWARD(long double, scalbnl, (long double x, int exponent), (x, exponent))
FORWARD(long double, scalblnl, (long double x, long exponent), (x, exponent))
FORWARD(long double, modfl, (long double x, long double *whole), (x, whole))
FORWARD(long double, remquol, (long double x, long double y, int *quotient),
        (x, y, quotient))
FORWARD(long double, nanl, (const char *tag), (tag))
#endif
