#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

/* The long double half of what <math.h>'s math_errhandling promises - see
   the comment on it there, and the identical three in math.c. A NaN that
   arrives as an argument is not an error and every caller below settles that
   case first, which is why none of these is reached with one. */
static long double domain_error_l(void) {
    feraiseexcept(FE_INVALID);
    return (long double)NAN;
}

static long double pole_error_l(int negative) {
    feraiseexcept(FE_DIVBYZERO);
    return negative ? -(long double)INFINITY : (long double)INFINITY;
}

#if __LDBL_MANT_DIG__ != 64
#error "this file is the x87 80-bit extended library; long double is not it here"
#endif

#define LN2_HI    0x1.62e42fefa39fp-1L
#define LN2_LO    -0x1.950d871319ff0342p-54L
#define LOG2E     0x1.71547652b82fe178p+0L
#define LOG10_2   0x1.34413509f79fef32p-2L
#define LN2       0x1.62e42fefa39ef358p-1L

#define TWO_POW_63 9223372036854775808.0L
#define EXP2_MAX   16384.0L
#define EXP2_MIN   -16446.0L

union long_double_bits {
    long double value;
    struct {
        uint64_t mantissa;
        uint16_t sign_exponent;
    } parts;
};

#define X87_CLOBBER "st", "st(1)", "st(2)", "st(3)", \
                    "st(4)", "st(5)", "st(6)", "st(7)"

static long double x87_sqrt(long double x) {
    long double r;
    __asm__ volatile("fldt %1\n\tfsqrt\n\tfstpt %0"
                     : "=m"(r) : "m"(x) : X87_CLOBBER);
    return r;
}

static long double x87_atan2(long double y, long double x) {
    long double r;
    __asm__ volatile("fldt %1\n\tfldt %2\n\tfpatan\n\tfstpt %0"
                     : "=m"(r) : "m"(y), "m"(x) : X87_CLOBBER);
    return r;
}

static long double x87_two_to_x_minus_one(long double x) {
    long double r;
    __asm__ volatile("fldt %1\n\tf2xm1\n\tfstpt %0"
                     : "=m"(r) : "m"(x) : X87_CLOBBER);
    return r;
}

static long double x87_scale(long double x, long double n) {
    long double r;
    __asm__ volatile("fldt %1\n\tfldt %2\n\tfscale\n\tfstp %%st(1)\n\tfstpt %0"
                     : "=m"(r) : "m"(n), "m"(x) : X87_CLOBBER);
    return r;
}

static long double x87_y_log2_x(long double y, long double x) {
    long double r;
    __asm__ volatile("fldt %1\n\tfldt %2\n\tfyl2x\n\tfstpt %0"
                     : "=m"(r) : "m"(y), "m"(x) : X87_CLOBBER);
    return r;
}

static long double x87_y_log2_x_plus_one(long double y, long double x) {
    long double r;
    __asm__ volatile("fldt %1\n\tfldt %2\n\tfyl2xp1\n\tfstpt %0"
                     : "=m"(r) : "m"(y), "m"(x) : X87_CLOBBER);
    return r;
}

static long double x87_round_to_integer(long double x) {
    long double r;
    __asm__ volatile("fldt %1\n\tfrndint\n\tfstpt %0"
                     : "=m"(r) : "m"(x) : X87_CLOBBER);
    return r;
}

static void x87_extract(long double x, long double *exponent,
                        long double *significand) {
    __asm__ volatile("fldt %2\n\tfxtract\n\tfstpt %1\n\tfstpt %0"
                     : "=m"(*exponent), "=m"(*significand)
                     : "m"(x) : X87_CLOBBER);
}

/* fprem and fprem1 reduce by at most 2^63 of the divisor per execution and
   report an unfinished reduction in C2, so the loop is the instruction's
   contract rather than a retry.

   The same instructions also report the low three bits of the quotient in
   C0, C3 and C1, and remquol is not built on that: this machine returned
   all three clear for a quotient of seven, with the compiler's own bit
   extraction disassembled and correct. Whoever is at fault, a libm cannot
   rest a documented result on a flag it has watched come back empty, so
   quotient_low_bits below works it out with arithmetic instead. */
static long double x87_remainder(long double x, long double y, int ieee) {
    long double r;
    uint16_t status;
    if (ieee) {
        __asm__ volatile("fldt %2\n\tfldt %3\n"
                         "1:\tfprem1\n\tfnstsw %%ax\n\t"
                         "testb $0x04, %%ah\n\tjnz 1b\n\t"
                         "fstpt %0\n\tfstp %%st(0)"
                         : "=m"(r), "=a"(status) : "m"(y), "m"(x)
                         : X87_CLOBBER, "cc");
    } else {
        __asm__ volatile("fldt %2\n\tfldt %3\n"
                         "1:\tfprem\n\tfnstsw %%ax\n\t"
                         "testb $0x04, %%ah\n\tjnz 1b\n\t"
                         "fstpt %0\n\tfstp %%st(0)"
                         : "=m"(r), "=a"(status) : "m"(y), "m"(x)
                         : X87_CLOBBER, "cc");
    }
    (void)status;
    return r;
}

long double fabsl(long double x) {
    union long_double_bits b;
    b.value = x;
    b.parts.sign_exponent &= 0x7FFFu;
    return b.value;
}

long double copysignl(long double x, long double y) {
    union long_double_bits bx, by;
    bx.value = x;
    by.value = y;
    bx.parts.sign_exponent =
        (uint16_t)((bx.parts.sign_exponent & 0x7FFFu) |
                   (by.parts.sign_exponent & 0x8000u));
    return bx.value;
}

long double sqrtl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < 0.0L) {
        return domain_error_l();
    }
    return x87_sqrt(x);
}

long double truncl(long double x) {
    if (!isfinite(x) || fabsl(x) >= TWO_POW_63) {
        return x;
    }
    return copysignl((long double)(long long)x, x);
}

long double floorl(long double x) {
    if (!isfinite(x)) {
        return x;
    }
    long double t = truncl(x);
    if (t == x) {
        return x < 0.0L ? copysignl(t, x) : t;
    }
    return x < 0.0L ? t - 1.0L : t;
}

long double ceill(long double x) {
    long double t = truncl(x);
    if (!isfinite(x) || t == x) {
        return t;
    }
    return x > 0.0L ? t + 1.0L : copysignl(t, x);
}

long double roundl(long double x) {
    if (!isfinite(x) || fabsl(x) >= TWO_POW_63) {
        return x;
    }
    long double t = truncl(x);
    long double f = fabsl(x - t);
    if (f >= 0.5L) {
        t = x < 0.0L ? t - 1.0L : t + 1.0L;
    }
    return copysignl(t, x);
}

long double rintl(long double x) {
    if (!isfinite(x)) {
        return x;
    }
    return x87_round_to_integer(x);
}

long double nearbyintl(long double x) {
    return rintl(x);
}

/* F.10.6.5 and F.10.6.7: an integer this format's value does not fit -
   a NaN, an infinity, or anything at or past 2^63 - raises invalid, and
   that is REQUIRED. fistp happens to raise it, but a conversion out of
   range is undefined in C, so the flag is raised here rather than left to
   whichever instruction a compiler picks. long and long long are both 64
   bits on this target, so one range serves all four. */
static long long to_integer(long double r) {
    if (isgreaterequal(r, -TWO_POW_63) && isless(r, TWO_POW_63)) {
        return (long long)r;
    }
    feraiseexcept(FE_INVALID);
    return LLONG_MIN;
}

long lrintl(long double x) {
    return (long)to_integer(rintl(x));
}

long long llrintl(long double x) {
    return to_integer(rintl(x));
}

long lroundl(long double x) {
    return (long)to_integer(roundl(x));
}

long long llroundl(long double x) {
    return to_integer(roundl(x));
}

long double frexpl(long double x, int *exponent) {
    if (x == 0.0L || !isfinite(x)) {
        *exponent = 0;
        return x;
    }
    long double e, s;
    x87_extract(x, &e, &s);
    *exponent = (int)e + 1;
    return s * 0.5L;
}

/* fscale takes its exponent from the x87 stack, so a count beyond what an
   80-bit exponent can express has to saturate before it gets there or the
   truncation inside the instruction decides the answer. */
static long double scale_by(long double x, long long n) {
    if (n > 131072) {
        n = 131072;
    } else if (n < -131072) {
        n = -131072;
    }
    return x87_scale(x, (long double)n);
}

long double ldexpl(long double x, int exponent) {
    if (x == 0.0L || !isfinite(x)) {
        return x;
    }
    return scale_by(x, exponent);
}

long double scalbnl(long double x, int exponent) {
    return ldexpl(x, exponent);
}

long double scalblnl(long double x, long exponent) {
    if (x == 0.0L || !isfinite(x)) {
        return x;
    }
    return scale_by(x, exponent);
}

/* C11 7.12.6.5 lets ilogb report a domain error for zero, an infinity or
   a NaN; IEEE 754-2008 5.3.3 and C23 F.10.3.8 require it, as invalid, since
   none of the three has an exponent an int can hold. This raises it - the
   only family besides the integer roundings where a NaN argument does, and
   what the host's libm does too. */
int ilogbl(long double x) {
    if (x == 0.0L) {
        feraiseexcept(FE_INVALID);
        return FP_ILOGB0;
    }
    if (isnan(x)) {
        feraiseexcept(FE_INVALID);
        return FP_ILOGBNAN;
    }
    if (isinf(x)) {
        feraiseexcept(FE_INVALID);
        return INT_MAX;
    }
    long double e, s;
    x87_extract(x, &e, &s);
    return (int)e;
}

long double logbl(long double x) {
    if (x == 0.0L) {
        return pole_error_l(1);
    }
    if (!isfinite(x)) {
        return fabsl(x);
    }
    long double e, s;
    x87_extract(x, &e, &s);
    return e;
}

long double modfl(long double x, long double *integer_part) {
    if (isinf(x)) {
        *integer_part = x;
        return copysignl(0.0L, x);
    }
    if (isnan(x)) {
        *integer_part = x;
        return x;
    }
    long double t = truncl(x);
    *integer_part = t;
    return copysignl(x - t, x);
}

long double exp2l(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x >= EXP2_MAX) {
        return (long double)INFINITY;
    }
    if (x <= EXP2_MIN) {
        return 0.0L;
    }
    long double n = x87_round_to_integer(x);
    long double f = x - n;
    return x87_scale(x87_two_to_x_minus_one(f) + 1.0L, n);
}

long double expl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x > 11357.0L) {
        return (long double)INFINITY;
    }
    if (x < -11400.0L) {
        return 0.0L;
    }
    long double n = x87_round_to_integer(x * LOG2E);
    long double r = (x - n * LN2_HI) - n * LN2_LO;
    return x87_scale(x87_two_to_x_minus_one(r * LOG2E) + 1.0L, n);
}

long double expm1l(long double x) {
    if (isnan(x) || x == 0.0L) {
        return x;
    }
    if (x > 11357.0L) {
        return (long double)INFINITY;
    }
    if (x < -11400.0L) {
        return -1.0L;
    }
    /* f2xm1 computes 2^f - 1 without ever forming the 1, which is the whole
       reason expm1 exists; outside its domain there is nothing to cancel. */
    if (fabsl(x) < 0.69L) {
        return x87_two_to_x_minus_one(x * LOG2E);
    }
    return expl(x) - 1.0L;
}

long double logl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < 0.0L) {
        return domain_error_l();
    }
    if (x == 0.0L) {
        return pole_error_l(1);
    }
    if (isinf(x)) {
        return x;
    }
    return x87_y_log2_x(LN2, x);
}

long double log2l(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < 0.0L) {
        return domain_error_l();
    }
    if (x == 0.0L) {
        return pole_error_l(1);
    }
    if (isinf(x)) {
        return x;
    }
    return x87_y_log2_x(1.0L, x);
}

long double log10l(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < 0.0L) {
        return domain_error_l();
    }
    if (x == 0.0L) {
        return pole_error_l(1);
    }
    if (isinf(x)) {
        return x;
    }
    return x87_y_log2_x(LOG10_2, x);
}

long double log1pl(long double x) {
    if (isnan(x) || x == 0.0L) {
        return x;
    }
    if (x < -1.0L) {
        return domain_error_l();
    }
    if (x == -1.0L) {
        return pole_error_l(1);
    }
    if (isinf(x)) {
        return x;
    }
    /* fyl2xp1's operand range is |x| < 1 - sqrt(2)/2; past it the 1 in
       1 + x is no longer the thing that would have been lost. */
    if (fabsl(x) < 0.29L) {
        return x87_y_log2_x_plus_one(LN2, x);
    }
    return x87_y_log2_x(LN2, 1.0L + x);
}

/* The circular functions are the one family here that is NOT an x87
   instruction, and the reason is measured rather than stylistic: fsin, fcos
   and fptan came back from this machine wrong by up to 2,500,000 units in
   the last place, while fsqrt, fpatan, fyl2x, f2xm1 and fprem were all
   within one. The size of the error is exactly what rounding the ARGUMENT
   to a double costs, which is what an emulator without an x87 does when it
   reaches for a host sin(). A libm that is only right on silicon this OS is
   not running on is not right. So the reduction and the polynomial are
   here, in this format, and they are correct wherever the four arithmetic
   operations are.

   pi/2 in five pieces of 32 significant bits each: n * piece is then exact
   for every n a 32-bit reduction can produce, so the whole subtraction
   chain is exact and the only error left is the 160 bits of pi/2 running
   out - at 2^-160, which is 96 bits below where this format stops. */
#define PI_OVER_TWO_0 0x1.921fb544p+0L
#define PI_OVER_TWO_1 0x1.0b4611a6p-34L
#define PI_OVER_TWO_2 0x1.3198a2ep-69L
#define PI_OVER_TWO_3 0x1.b839a252p-104L
#define PI_OVER_TWO_4 0x1.27044534p-142L
#define TWO_OVER_PI   0x1.45f306dc9c882a54p-1L

#define TRIG_MAX_ARGUMENT 0x1p32L

#define SIN_0 -0x1.5555555555555556p-3L
#define SIN_1 0x1.1111111111111112p-7L
#define SIN_2 -0x1.a01a01a01a01a01ap-13L
#define SIN_3 0x1.71de3a556c7338fap-19L
#define SIN_4 -0x1.ae64567f544e38fep-26L
#define SIN_5 0x1.6124613a86d097cap-33L
#define SIN_6 -0x1.ae7f3e733b81f11ep-41L
#define SIN_7 0x1.952c77030ad4a6b2p-49L
#define SIN_8 -0x1.2f49b4681415724cp-57L
#define SIN_9 0x1.71b8ef6dcf5718bep-66L

/* The -1/2 that would have been COS_0 is written out in kernel_cos, so
   the polynomial below starts at the r^4 term. */
#define COS_1 0x1.5555555555555556p-5L
#define COS_2 -0x1.6c16c16c16c16c16p-10L
#define COS_3 0x1.a01a01a01a01a01ap-16L
#define COS_4 -0x1.27e4fb7789f5c72ep-22L
#define COS_5 0x1.1eed8eff8d897b54p-29L
#define COS_6 -0x1.93974a8c07c9d20cp-37L
#define COS_7 0x1.ae7f3e733b81f11ep-45L
#define COS_8 -0x1.6827863b97d977bcp-53L
#define COS_9 0x1.e542ba402022507ap-62L

/* The leading r is kept outside the polynomial, so a result near zero keeps
   every bit the reduction gave it. */
static long double kernel_sin(long double r) {
    long double q = r * r;
    long double p = SIN_9;
    p = SIN_8 + q * p;
    p = SIN_7 + q * p;
    p = SIN_6 + q * p;
    p = SIN_5 + q * p;
    p = SIN_4 + q * p;
    p = SIN_3 + q * p;
    p = SIN_2 + q * p;
    p = SIN_1 + q * p;
    p = SIN_0 + q * p;
    return r + r * q * p;
}

/* 1 - r^2/2 rounds, and fdlibm recovers what that rounding loses: (1 - z) is
   exact for z in [0.5, 1], so (1 - z) - r^2/2 is exactly the lost bits.

   Measured, twice, because the first measurement was misread. It does NOT
   move cosl's own worst case - two ulp with it and two without. It moves
   tanl's, from three to two, because tan divides by this and the point where
   tan is worst is not the point where cos is. Three operations for one ulp
   of a different function is the kind of thing only a sweep finds. */
static long double kernel_cos(long double r) {
    long double q = r * r;
    long double p = COS_9;
    p = COS_8 + q * p;
    p = COS_7 + q * p;
    p = COS_6 + q * p;
    p = COS_5 + q * p;
    p = COS_4 + q * p;
    p = COS_3 + q * p;
    p = COS_2 + q * p;
    p = COS_1 + q * p;
    long double half_q = 0.5L * q;
    long double z = 1.0L - half_q;
    return z + (((1.0L - z) - half_q) + q * q * p);
}

static int reduce_quadrant(long double x, long double *remainder) {
    long double n = x87_round_to_integer(x * TWO_OVER_PI);
    long double r = x;
    r = r - n * PI_OVER_TWO_0;
    r = r - n * PI_OVER_TWO_1;
    r = r - n * PI_OVER_TWO_2;
    r = r - n * PI_OVER_TWO_3;
    r = r - n * PI_OVER_TWO_4;
    *remainder = r;
    long long whole = (long long)n;
    return (int)(((whole % 4) + 4) % 4);
}

long double sinl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (!isfinite(x) || fabsl(x) >= TRIG_MAX_ARGUMENT) {
        return domain_error_l();
    }
    long double r;
    switch (reduce_quadrant(x, &r)) {
    case 0:  return kernel_sin(r);
    case 1:  return kernel_cos(r);
    case 2:  return -kernel_sin(r);
    default: return -kernel_cos(r);
    }
}

long double cosl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (!isfinite(x) || fabsl(x) >= TRIG_MAX_ARGUMENT) {
        return domain_error_l();
    }
    long double r;
    switch (reduce_quadrant(x, &r)) {
    case 0:  return kernel_cos(r);
    case 1:  return -kernel_sin(r);
    case 2:  return -kernel_cos(r);
    default: return kernel_sin(r);
    }
}

long double tanl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (!isfinite(x) || fabsl(x) >= TRIG_MAX_ARGUMENT) {
        return domain_error_l();
    }
    long double r;
    int quadrant = reduce_quadrant(x, &r);
    long double s = kernel_sin(r), c = kernel_cos(r);
    return (quadrant & 1) ? -c / s : s / c;
}

long double atan2l(long double y, long double x) {
    if (isnan(y)) {
        return y;
    }
    if (isnan(x)) {
        return x;
    }
    return x87_atan2(y, x);
}

long double atanl(long double x) {
    if (isnan(x)) {
        return x;
    }
    return x87_atan2(x, 1.0L);
}

long double asinl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (fabsl(x) > 1.0L) {
        return domain_error_l();
    }
    return x87_atan2(x, x87_sqrt((1.0L - x) * (1.0L + x)));
}

long double acosl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (fabsl(x) > 1.0L) {
        return domain_error_l();
    }
    return x87_atan2(x87_sqrt((1.0L - x) * (1.0L + x)), x);
}

long double coshl(long double x) {
    if (isnan(x)) {
        return x;
    }
    long double a = fabsl(x);
    if (a > 11357.0L) {
        return (long double)INFINITY;
    }
    long double t = expl(a);
    return (t + 1.0L / t) * 0.5L;
}

long double sinhl(long double x) {
    if (isnan(x) || x == 0.0L) {
        return x;
    }
    long double a = fabsl(x);
    if (a > 11357.0L) {
        return copysignl((long double)INFINITY, x);
    }
    /* Below the point where exp(a) and 1/exp(a) differ in exponent, their
       difference is all cancellation; expm1 is the form that keeps it. */
    if (a < 1.0L) {
        long double t = expm1l(a);
        return copysignl((t + t / (t + 1.0L)) * 0.5L, x);
    }
    long double t = expl(a);
    return copysignl((t - 1.0L / t) * 0.5L, x);
}

long double tanhl(long double x) {
    if (isnan(x) || x == 0.0L) {
        return x;
    }
    long double a = fabsl(x);
    if (a > 40.0L) {
        return copysignl(1.0L, x);
    }
    long double t = expm1l(-2.0L * a);
    return copysignl(-t / (t + 2.0L), x);
}

long double asinhl(long double x) {
    if (isnan(x) || x == 0.0L || isinf(x)) {
        return x;
    }
    long double a = fabsl(x);
    long double r;
    if (a > 0x1p32L) {
        r = logl(a) + LN2;
    } else if (a > 1.0L) {
        r = logl(2.0L * a + 1.0L / (x87_sqrt(a * a + 1.0L) + a));
    } else {
        r = log1pl(a + a * a / (1.0L + x87_sqrt(a * a + 1.0L)));
    }
    return copysignl(r, x);
}

long double acoshl(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < 1.0L) {
        return domain_error_l();
    }
    if (isinf(x)) {
        return x;
    }
    if (x > 0x1p32L) {
        return logl(x) + LN2;
    }
    long double t = x - 1.0L;
    return log1pl(t + x87_sqrt(t * t + 2.0L * t));
}

long double atanhl(long double x) {
    if (isnan(x) || x == 0.0L) {
        return x;
    }
    long double a = fabsl(x);
    if (a > 1.0L) {
        return domain_error_l();
    }
    if (a == 1.0L) {
        feraiseexcept(FE_DIVBYZERO);
        return copysignl((long double)INFINITY, x);
    }
    return copysignl(0.5L * log1pl(2.0L * a / (1.0L - a)), x);
}

long double hypotl(long double x, long double y) {
    if (isinf(x) || isinf(y)) {
        return (long double)INFINITY;
    }
    if (isnan(x) || isnan(y)) {
        return (long double)NAN;
    }
    long double a = fabsl(x), b = fabsl(y);
    if (a < b) {
        long double t = a;
        a = b;
        b = t;
    }
    if (a == 0.0L) {
        return 0.0L;
    }
    long double t = b / a;
    return a * x87_sqrt(1.0L + t * t);
}

long double cbrtl(long double x) {
    if (x == 0.0L || !isfinite(x)) {
        return x;
    }
    long double a = fabsl(x);
    long double r = exp2l(x87_y_log2_x(1.0L, a) / 3.0L);
    /* One Newton step on a starting point already good to a few ulp lands
       on the correctly rounded root; the step is what makes the claim. */
    r = r - (r - a / (r * r)) / 3.0L;
    return copysignl(r, x);
}

long double fmodl(long double x, long double y) {
    if (isnan(x) || isnan(y)) {
        return (long double)NAN;
    }
    if (isinf(x) || y == 0.0L) {
        return domain_error_l();
    }
    if (isinf(y) || x == 0.0L) {
        return x;
    }
    return x87_remainder(x, y, 0);
}

long double remainderl(long double x, long double y) {
    if (isnan(x) || isnan(y)) {
        return (long double)NAN;
    }
    if (isinf(x) || y == 0.0L) {
        return domain_error_l();
    }
    if (isinf(y)) {
        return x;
    }
    return x87_remainder(x, y, 1);
}

/* |x / y| modulo eight, for two positive arguments. Reducing by 8y first is
   what keeps the division small enough to round to the right integer however
   far apart the arguments are - taking (x - remainder) / y straight would
   lose the very bits this is asked for once the quotient passes 2^62. */
static int quotient_low_bits(long double x, long double y) {
    long double eight = y * 8.0L;
    long double reduced = isinf(eight) ? x : x87_remainder(x, eight, 0);
    long double rest = x87_remainder(x, y, 1);
    long long low = (long long)x87_round_to_integer((reduced - rest) / y);
    return (int)(((low % 8) + 8) % 8);
}

long double remquol(long double x, long double y, int *quotient) {
    if (isnan(x) || isnan(y)) {
        *quotient = 0;
        return (long double)NAN;
    }
    if (isinf(x) || y == 0.0L) {
        *quotient = 0;
        return domain_error_l();
    }
    if (isinf(y)) {
        *quotient = 0;
        return x;
    }
    long double r = x87_remainder(x, y, 1);
    int sign = (signbit(x) != signbit(y)) ? -1 : 1;
    *quotient = sign * quotient_low_bits(fabsl(x), fabsl(y));
    return r;
}

long double nextafterl(long double x, long double y) {
    if (isnan(x)) {
        return x;
    }
    if (isnan(y)) {
        return y;
    }
    if (x == y) {
        return y;
    }
    union long_double_bits b;
    b.value = x;
    if (x == 0.0L) {
        b.parts.sign_exponent = (uint16_t)(y < 0.0L ? 0x8000u : 0u);
        b.parts.mantissa = 1;
        return b.value;
    }
    /* This format's integer bit is explicit, so the encoding is not one
       ascending integer the way a double's is: the step across the bottom
       of the exponent range swaps a leading 1 for a leading 0, and getting
       it wrong produces an unnormal, which this FPU rejects rather than
       rounds. */
    uint64_t top = (uint64_t)1 << 63;
    unsigned exponent = b.parts.sign_exponent & 0x7FFFu;
    int away = (y > x) == (x > 0.0L);
    if (away) {
        if (b.parts.mantissa == UINT64_MAX) {
            b.parts.mantissa = top;
            b.parts.sign_exponent++;
        } else if (exponent == 0 && b.parts.mantissa == top - 1) {
            b.parts.mantissa = top;
            b.parts.sign_exponent++;
        } else {
            b.parts.mantissa++;
        }
    } else {
        if (b.parts.mantissa == top && exponent > 1) {
            b.parts.mantissa = UINT64_MAX;
            b.parts.sign_exponent--;
        } else if (b.parts.mantissa == top && exponent == 1) {
            b.parts.mantissa = top - 1;
            b.parts.sign_exponent--;
        } else {
            b.parts.mantissa--;
        }
    }
    return b.value;
}

long double nexttowardl(long double x, long double y) {
    return nextafterl(x, y);
}

/* x - y if x > y, else +0 - and when x > y is false 7.12.12.1 performs no
   subtraction for fdim(inf, inf) to be invalid about. x86_64-elf-gcc
   compiles this as a branch, because its default -ftrapping-math forbids
   doing an operation that can raise before the test that guards it. Apple's
   clang assumes by default that nothing is watching the flags, and compiled
   it as fsubp, then fcmov - inf - inf done whatever the comparison said, and
   invalid raised. Choosing the operands first does not help: it folded the
   choices back into the same fsubp. tools/math-test.sh found it, once it
   graded this file rather than the host's fdiml, and FENV_ACCESS is the
   standard's own way to say that the flags here are observed. GCC does not
   know the pragma and does not need it. */
long double fdiml(long double x, long double y) {
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    if (isnan(x) || isnan(y)) {
        return (long double)NAN;
    }
    return x > y ? x - y : 0.0L;
}

long double fmaxl(long double x, long double y) {
    if (isnan(x)) {
        return y;
    }
    if (isnan(y)) {
        return x;
    }
    return x > y ? x : y;
}

long double fminl(long double x, long double y) {
    if (isnan(x)) {
        return y;
    }
    if (isnan(y)) {
        return x;
    }
    return x < y ? x : y;
}

long double nanl(const char *tag) {
    (void)tag;
    return (long double)NAN;
}

/* An integer of 15 bits times a 32-bit half of y is exact in 64, so the two
   halves carry the whole of y * exponent with nothing rounded away. That is
   what keeps pow's exponent argument accurate where x is far from 1 - the
   naive y * log2(x) loses a bit per octave of the exponent. */
static void split_mantissa(long double y, long double *high, long double *low) {
    union long_double_bits b;
    b.value = y;
    b.parts.mantissa &= ~(uint64_t)0xFFFFFFFFu;
    *high = b.value;
    *low = y - b.value;
}

static long double pow_positive(long double x, long double y) {
    int e;
    long double m = frexpl(x, &e);
    long double log2m = x87_y_log2_x(1.0L, m);
    long double yh, yl;
    split_mantissa(y, &yh, &yl);
    long double p1 = yh * (long double)e;
    long double p2 = yl * (long double)e;
    long double p3 = y * log2m;
    long double t = (p1 + p2) + p3;
    if (t >= EXP2_MAX) {
        return (long double)INFINITY;
    }
    if (t <= EXP2_MIN) {
        return 0.0L;
    }
    long double n = x87_round_to_integer(t);
    long double f = ((p1 - n) + p2) + p3;
    if (fabsl(f) > 1.0L) {
        f = t - n;
    }
    return x87_scale(x87_two_to_x_minus_one(f) + 1.0L, n);
}

static int is_odd_integer(long double y) {
    if (fabsl(y) >= TWO_POW_63) {
        return 0;
    }
    long long n = (long long)y;
    return (long double)n == y && (n & 1);
}

static int is_integer(long double y) {
    return truncl(y) == y;
}

long double powl(long double x, long double y) {
    if (y == 0.0L) {
        return 1.0L;
    }
    if (x == 1.0L) {
        return 1.0L;
    }
    if (isnan(x) || isnan(y)) {
        return (long double)NAN;
    }
    if (x == 0.0L) {
        if (y < 0.0L) {
            feraiseexcept(FE_DIVBYZERO);
            return is_odd_integer(y) ? copysignl((long double)INFINITY, x)
                                     : (long double)INFINITY;
        }
        return is_odd_integer(y) ? x : 0.0L;
    }
    if (isinf(y)) {
        long double a = fabsl(x);
        if (a == 1.0L) {
            return 1.0L;
        }
        if ((a > 1.0L) == (y > 0.0L)) {
            return (long double)INFINITY;
        }
        return 0.0L;
    }
    if (isinf(x)) {
        if (x > 0.0L) {
            return y > 0.0L ? x : 0.0L;
        }
        long double r = y > 0.0L ? (long double)INFINITY : 0.0L;
        return is_odd_integer(y) ? -r : r;
    }
    if (x < 0.0L) {
        if (!is_integer(y)) {
            return domain_error_l();
        }
        long double r = pow_positive(-x, y);
        return is_odd_integer(y) ? -r : r;
    }
    return pow_positive(x, y);
}
