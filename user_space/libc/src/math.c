#include <fenv.h>
#include <limits.h>
#include <math.h>

/* math_errhandling says MATH_ERREXCEPT, and these three are what make that
   true - see the comment on it in <math.h>. Every arithmetic path raises on
   its own because the hardware does it; these are for the errors this library
   detects and returns from before any arithmetic runs.

   A NaN that ARRIVES as an argument is not an error and must stay quiet, so
   every caller below settles that case before it gets here. That distinction
   is the whole difference between a library that reports errors and one that
   reports arguments.

   "Settles that case" has to mean before any relational operator sees the
   NaN, or with a comparison that cannot signal. C's x < y is a SIGNALING
   comparison in IEC 60559's terms, and x86_64-elf-gcc compiles it as COMISD
   (and fcomi for a long double), which raises FE_INVALID for a quiet NaN -
   so `if (x < 0.0)` in front of a NaN was itself the domain error this
   library reported. On the machine that was fabs, sqrt, trunc, round,
   modf, asin, acos, atan, atan2, pow, hypot and copysign, their float
   twins, and floorl - 30 calls of /bin/mathltest's 214. Apple's clang on an
   arm64 host emits a quiet fcmp for the same source, which is why the host
   test had never seen it; on an x86 host it emits cmpnltsd and saw asin.
   So a guard here is either NaN-first, a bit operation, or one of the
   isless family, which C defines as quiet. Equality is quiet already
   (UCOMISD, fucomi). */
static double domain_error(void) {
    feraiseexcept(FE_INVALID);
    return NAN;
}

static double pole_error(int negative) {
    feraiseexcept(FE_DIVBYZERO);
    return negative ? -HUGE_VAL : HUGE_VAL;
}

static double overflow_error(int negative) {
    feraiseexcept(FE_OVERFLOW | FE_INEXACT);
    return negative ? -HUGE_VAL : HUGE_VAL;
}

/* Annex F calls fabs and copysign bit operations, and they are written as
   bit operations: neither may raise anything, and neither may lose the sign
   of a zero or a NaN. */
union double_bits {
    double value;
    unsigned long long bits;
};

#define DOUBLE_SIGN_BIT 0x8000000000000000ull

double fabs(double x) {
    union double_bits b = {x};
    b.bits &= ~DOUBLE_SIGN_BIT;
    return b.value;
}

double sqrt(double x) {
    if (isless(x, 0.0)) {
        return domain_error();
    }
#if defined(__x86_64__)
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    return __builtin_sqrt(x);
#endif
}

#define TWO_POW_52 4503599627370496.0

double floor(double x) {
    if (!isfinite(x) || fabs(x) >= TWO_POW_52) {
        return x;
    }
    double t = (double)(long long)x;
    /* The conversion has no negative zero, and floor(-0) is -0: a floor's
       sign is always its argument's, and so is a ceiling's. */
    return copysign((x < 0.0 && t != x) ? t - 1.0 : t, x);
}

double ceil(double x) {
    if (!isfinite(x) || fabs(x) >= TWO_POW_52) {
        return x;
    }
    double t = (double)(long long)x;
    return copysign((x > 0.0 && t != x) ? t + 1.0 : t, x);
}

double fmod(double x, double y) {
    if (isnan(x) || isnan(y)) {
        return NAN;
    }
    if (isinf(x) || y == 0.0) {
        return domain_error();
    }
    if (isinf(y) || x == 0.0) {
        return x;
    }
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay) {
        return x;
    }
    int ex, ey;
    frexp(ax, &ex);
    frexp(ay, &ey);
    double r = ax;
    double d = ldexp(ay, ex - ey);
    for (int k = ex - ey; k >= 0; k--) {
        if (r >= d) {
            r -= d;
        }
        d *= 0.5;
    }
    return x < 0.0 ? -r : r;
}

#define TWO_PI 6.283185307179586476925286766559
#define M_PI_4_ 0.78539816339744830962

#define PIO2_A 1.5703125
#define PIO2_B 4.83826794896619256404e-04
#define PIO2_C -2.50827880633416613471e-20

static double kernel_sin(double r) {
    double r2 = r * r;
    double term = r;
    double sum = r;
    for (int n = 1; n <= 8; n++) {
        term *= -r2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

static double kernel_cos(double r) {
    double r2 = r * r;
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n <= 8; n++) {
        term *= -r2 / (double)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

#define TRIG_MAX_ARGUMENT 4503599627370496.0

static int quadrant(double x, double *r) {
    double nd = x / M_PI_2;
    long long n = (long long)(nd >= 0.0 ? nd + 0.5 : nd - 0.5);
    double dn = (double)n;
    *r = ((x - dn * PIO2_A) - dn * PIO2_B) - dn * PIO2_C;
    return (int)(((n % 4) + 4) % 4);
}

double sin(double x) {
    if (isnan(x) || x == 0.0) {
        return x;
    }
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARGUMENT) {
        return domain_error();
    }
    double r;
    switch (quadrant(x, &r)) {
    case 0:  return kernel_sin(r);
    case 1:  return kernel_cos(r);
    case 2:  return -kernel_sin(r);
    default: return -kernel_cos(r);
    }
}

double cos(double x) {
    if (isnan(x)) {
        return x;
    }
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARGUMENT) {
        return domain_error();
    }
    double r;
    switch (quadrant(x, &r)) {
    case 0:  return kernel_cos(r);
    case 1:  return -kernel_sin(r);
    case 2:  return -kernel_cos(r);
    default: return kernel_sin(r);
    }
}

double tan(double x) {
    if (isnan(x) || x == 0.0) {
        return x;
    }
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARGUMENT) {
        return domain_error();
    }
    double c = cos(x);
    if (c == 0.0) {
        return pole_error(0);
    }
    return sin(x) / c;
}

double atan(double x) {
    if (isnan(x) || x == 0.0) {
        return x;
    }
    int neg = 0;
    if (x < 0.0) {
        neg = 1;
        x = -x;
    }
    int inverted = 0;
    if (x > 1.0) {
        inverted = 1;
        x = 1.0 / x;
    }
    int halved = 0;
    if (x > 0.42) {
        halved = 1;
        x = x / (1.0 + sqrt(1.0 + x * x));
    }
    double x2 = x * x;
    double term = x;
    double sum = x;
    for (int n = 1; n <= 20; n++) {
        term *= -x2;
        sum += term / (double)(2 * n + 1);
    }
    if (halved) {
        sum *= 2.0;
    }
    if (inverted) {
        sum = M_PI_2 - sum;
    }
    return neg ? -sum : sum;
}

double atan2(double y, double x) {
    if (isnan(x) || isnan(y)) {
        return x + y;
    }
    /* F.10.1.4: a zero y keeps its sign, and which of 0 and pi it is
       decided by the sign of x - including the sign of a zero x, which no
       comparison can see. */
    if (y == 0.0) {
        if (signbit(x)) {
            return copysign(M_PI, y);
        }
        return y;
    }
    if (isinf(y) && isinf(x)) {
        double d = (y > 0.0 ? 1.0 : -1.0) * (x > 0.0 ? M_PI_4_ : 3.0 * M_PI_4_);
        return d;
    }
    if (x > 0.0) {
        return atan(y / x);
    }
    if (x < 0.0) {
        return y >= 0.0 ? atan(y / x) + M_PI : atan(y / x) - M_PI;
    }
    if (y > 0.0) {
        return M_PI_2;
    }
    if (y < 0.0) {
        return -M_PI_2;
    }
    return 0.0;
}

double asin(double x) {
    if (isnan(x) || x == 0.0) {
        return x;
    }
    if (x > 1.0 || x < -1.0) {
        return domain_error();
    }
    if (x == 1.0) {
        return M_PI_2;
    }
    if (x == -1.0) {
        return -M_PI_2;
    }
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x) {
    return M_PI_2 - asin(x);
}

#define LN2 0.69314718055994530942

double exp(double x) {
    if (isnan(x)) {
        return x;
    }
    /* F.10.3.1: exp(+inf) is +inf exactly, which is not an overflow. */
    if (isinf(x)) {
        return signbit(x) ? 0.0 : x;
    }
    if (x > 709.0) {
        return overflow_error(0);
    }
    if (x < -745.0) {
        return 0.0;
    }
    double kd = x / LN2;
    long long k = (long long)(kd >= 0.0 ? kd + 0.5 : kd - 0.5);
    double r = x - (double)k * LN2;

    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n <= 16; n++) {
        term *= r / (double)n;
        sum += term;
    }

    double scale = 1.0;
    double base = (k < 0) ? 0.5 : 2.0;
    long long n = (k < 0) ? -k : k;
    while (n > 0) {
        if (n & 1) {
            scale *= base;
        }
        base *= base;
        n >>= 1;
    }
    return sum * scale;
}

double log(double x) {
    if (isnan(x) || (isinf(x) && x > 0.0)) {
        return x;
    }
    if (x < 0.0) {
        return domain_error();
    }
    if (x == 0.0) {
        return pole_error(1);
    }
    int k = 0;
    while (x > 1.4142135623730951) {
        x *= 0.5;
        k++;
    }
    while (x < 0.7071067811865476) {
        x *= 2.0;
        k--;
    }
    double t = (x - 1.0) / (x + 1.0);
    double t2 = t * t;
    double term = t;
    double sum = t;
    for (int n = 1; n <= 20; n++) {
        term *= t2;
        sum += term / (double)(2 * n + 1);
    }
    return 2.0 * sum + (double)k * LN2;
}

double log10(double x) {
    return log(x) / 2.302585092994045684;
}

#define TWO_POW_53 9007199254740992.0

/* An odd integer has a units bit, and no double at or past 2^53 does. */
static int is_odd_integer(double y) {
    if (!isless(fabs(y), TWO_POW_53)) {
        return 0;
    }
    long long n = (long long)y;
    return (double)n == y && (n & 1);
}

/* F.10.4.4 is a table, and every row of it is answered here before any
   logarithm is taken: exp(y * log(x)) is right for none of them, and for a
   negative base or an infinite exponent it was an invalid operation where
   the table has a number. powl answers the same table the same way. */
double pow(double x, double y) {
    if (y == 0.0) {
        return 1.0;
    }
    /* One to any power is one, a NaN power included - the other case where
       a NaN argument does not make a NaN result. */
    if (x == 1.0) {
        return 1.0;
    }
    if (isnan(x) || isnan(y)) {
        return x + y;
    }
    int odd = is_odd_integer(y);
    if (x == 0.0) {
        if (signbit(y)) {
            return pole_error(odd && signbit(x));
        }
        return odd ? x : 0.0;
    }
    if (isinf(y)) {
        double a = fabs(x);
        if (a == 1.0) {
            return 1.0;
        }
        return isgreater(a, 1.0) == !signbit(y) ? HUGE_VAL : 0.0;
    }
    if (isinf(x)) {
        double r = signbit(y) ? 0.0 : HUGE_VAL;
        return (signbit(x) && odd) ? -r : r;
    }
    if (signbit(x)) {
        if (trunc(y) != y) {
            return domain_error();
        }
        double magnitude = exp(y * log(-x));
        return odd ? -magnitude : magnitude;
    }
    return exp(y * log(x));
}

double frexp(double x, int *exp) {
    int e = 0;
    if (x == 0.0 || isnan(x) || isinf(x)) {
        if (exp) {
            *exp = 0;
        }
        return x;
    }
    double m = x < 0 ? -x : x;
    while (m >= 1.0) {
        m *= 0.5;
        e++;
    }
    while (m < 0.5) {
        m *= 2.0;
        e--;
    }
    if (exp) {
        *exp = e;
    }
    return x < 0 ? -m : m;
}

double ldexp(double x, int exp) {
    if (exp > 1100) {
        exp = 1100;
    } else if (exp < -1200) {
        exp = -1200;
    }
    double r = x;
    while (exp > 0) {
        r *= 2.0;
        exp--;
    }
    while (exp < 0) {
        r *= 0.5;
        exp++;
    }
    return r;
}

double trunc(double x) {
    return signbit(x) ? ceil(x) : floor(x);
}

double modf(double x, double *ipart) {
    double i = trunc(x);
    if (ipart) {
        *ipart = i;
    }
    /* inf - inf would be an invalid operation; F.10.3.12 says the
       fraction of an infinity is a zero of its sign. */
    if (isinf(x)) {
        return copysign(0.0, x);
    }
    return copysign(x - i, x);
}

double round(double x) {
    if (!isfinite(x)) {
        return x;
    }
    double t = trunc(x);
    double frac = x - t;
    if (frac >= 0.5) {
        return t + 1.0;
    }
    if (frac <= -0.5) {
        return t - 1.0;
    }
    return t;
}

/* F.10.6.7: a NaN, an infinity or a value past the integer type has no
   answer, and invalid is REQUIRED - the one place in this file a NaN
   argument raises. It is raised on purpose rather than left to whatever a
   compiler makes of the range check, which on one host is a signaling
   compare that raises and on another a quiet one that does not. */
long lround(double x) {
    double r = round(x);
    if (isgreaterequal(r, (double)LONG_MIN) && isless(r, (double)LONG_MAX)) {
        return (long)r;
    }
    feraiseexcept(FE_INVALID);
    return LONG_MIN;
}

long lroundf(float x) {
    return lround((double)x);
}

long long llround(double x) {
    double r = round(x);
    if (isgreaterequal(r, (double)LLONG_MIN) && isless(r, (double)LLONG_MAX)) {
        return (long long)r;
    }
    feraiseexcept(FE_INVALID);
    return LLONG_MIN;
}

long long llroundf(float x) {
    return llround((double)x);
}

double copysign(double x, double y) {
    union double_bits bx = {x}, by = {y};
    bx.bits = (bx.bits & ~DOUBLE_SIGN_BIT) | (by.bits & DOUBLE_SIGN_BIT);
    return bx.value;
}

double hypot(double x, double y) {
    /* F.10.4.3: an infinity wins over a NaN, so the infinity is asked
       about first. */
    if (isinf(x) || isinf(y)) {
        return HUGE_VAL;
    }
    if (isnan(x) || isnan(y)) {
        return x + y;
    }
    double ax = fabs(x);
    double ay = fabs(y);
    if (ax < ay) {
        double t = ax;
        ax = ay;
        ay = t;
    }
    if (ax == 0.0) {
        return 0.0;
    }
    double r = ay / ax;
    return ax * sqrt(1.0 + r * r);
}

double log2(double x) {
    return log(x) * 1.4426950408889634074;
}

double sinh(double x) {
    if (isnan(x) || isinf(x) || x == 0.0) {
        return x;
    }
    double e = exp(x);
    return (e - 1.0 / e) * 0.5;
}

double cosh(double x) {
    if (isnan(x)) {
        return x;
    }
    if (isinf(x)) {
        return HUGE_VAL;
    }
    double e = exp(x);
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x) {
    if (isnan(x) || x == 0.0) {
        return x;
    }
    if (x > 20.0) {
        return 1.0;
    }
    if (x < -20.0) {
        return -1.0;
    }
    double e = exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}

double fmax(double a, double b) {
    if (isnan(a)) {
        return b;
    }
    if (isnan(b)) {
        return a;
    }
    /* Annex F does not require fmax(-0, +0) to be +0, and says why (the
       footnote to F.10.9.2); here it costs one quiet comparison, so it is. */
    if (a == b) {
        return signbit(a) ? b : a;
    }
    return a > b ? a : b;
}

double fmin(double a, double b) {
    if (isnan(a)) {
        return b;
    }
    if (isnan(b)) {
        return a;
    }
    if (a == b) {
        return signbit(a) ? a : b;
    }
    return a < b ? a : b;
}

double expm1(double x) {
    if (isnan(x) || x == 0.0) {
        return x;
    }
    if (fabs(x) >= 1.0) {
        return exp(x) - 1.0;
    }
    double term = x;
    double sum = x;
    for (int n = 2; n <= 25; n++) {
        term *= x / (double)n;
        sum += term;
    }
    return sum;
}

double log1p(double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < -1.0) {
        return domain_error();
    }
    if (x == -1.0) {
        return pole_error(1);
    }
    if (fabs(x) >= 0.5) {
        return log(1.0 + x);
    }
    double s = x / (2.0 + x);
    double s2 = s * s;
    double term = s;
    double sum = s;
    for (int n = 3; n <= 41; n += 2) {
        term *= s2;
        sum += term / (double)n;
    }
    return 2.0 * sum;
}

double asinh(double x) {
    if (isnan(x) || isinf(x) || x == 0.0) {
        return x;
    }
    double ax = fabs(x);
    double r;
    if (ax > 1e8) {
        r = log(ax) + 0.69314718055994530942;
    } else {
        r = log1p(ax + ax * ax / (1.0 + sqrt(ax * ax + 1.0)));
    }
    return x < 0.0 ? -r : r;
}

double acosh(double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < 1.0) {
        return domain_error();
    }
    if (x > 1e8) {
        return log(x) + 0.69314718055994530942;
    }
    double t = x - 1.0;
    return log1p(t + sqrt(t * t + 2.0 * t));
}

double atanh(double x) {
    if (isnan(x) || x == 0.0) {
        return x;
    }
    double ax = fabs(x);
    if (ax > 1.0) {
        return domain_error();
    }
    if (ax == 1.0) {
        return pole_error(x < 0.0);
    }
    double r = 0.5 * log1p(2.0 * ax / (1.0 - ax));
    return x < 0.0 ? -r : r;
}

#define M_2_SQRTPI_ 1.12837916709551257390
#define M_1_SQRTPI_ 0.56418958354775628695

static double erf_series(double x) {
    double t = x * x;
    double term = x;
    double sum = x;
    for (int n = 1; n < 128; n++) {
        term *= 2.0 * t / (2.0 * (double)n + 1.0);
        sum += term;
        if (fabs(term) < fabs(sum) * 1e-18) {
            break;
        }
    }
    return M_2_SQRTPI_ * exp(-t) * sum;
}

static double erfc_cf(double x) {
    double cf = 0.0;
    for (int n = 40; n >= 1; n--) {
        cf = 0.5 * (double)n / (x + cf);
    }
    return exp(-x * x) * M_1_SQRTPI_ / (x + cf);
}

double erf(double x) {
    if (isnan(x)) {
        return x;
    }
    double ax = fabs(x);
    if (ax < 2.0) {
        return erf_series(x);
    }
    if (ax > 6.0) {
        return x > 0.0 ? 1.0 : -1.0;
    }
    double e = erfc_cf(ax);
    return x > 0.0 ? 1.0 - e : e - 1.0;
}

double erfc(double x) {
    if (isnan(x)) {
        return x;
    }
    if (x < 2.0) {
        return 1.0 - erf(x);
    }
    if (x > 27.0) {
        return 0.0;
    }
    return erfc_cf(x);
}

/* Which way to step is decided on the bits, as sign-magnitude integers,
   rather than by comparing the doubles - and not for want of a NaN guard,
   which is above. Apple's clang on an x86 host compiled nextafterf's two
   comparisons into one CMPLTPS across the whole xmm register, whose other
   three lanes hold whatever the caller left there; a stale NaN in one of
   them raised FE_INVALID for nextafterf(1, inf). isgreater did not stop it:
   once the compiler has proved both operands ordered it treats the quiet
   comparison as the signaling one, because by default it does not model
   the flags at all. An integer comparison has no flags to raise. */
static long long double_order(double x) {
    union double_bits b = {x};
    long long magnitude = (long long)(b.bits & ~DOUBLE_SIGN_BIT);
    return (b.bits & DOUBLE_SIGN_BIT) ? -magnitude : magnitude;
}

static int float_order(float x) {
    union { float f; unsigned int u; } b = {x};
    int magnitude = (int)(b.u & 0x7FFFFFFFu);
    return (b.u & 0x80000000u) ? -magnitude : magnitude;
}

double nextafter(double x, double y) {
    if (isnan(x) || isnan(y)) {
        return x + y;
    }
    if (x == y) {
        return y;
    }
    union { double d; unsigned long long u; } v;
    if (x == 0.0) {
        v.u = 1;
        return signbit(y) ? -v.d : v.d;
    }
    v.d = x;
    long long from = double_order(x);
    if ((double_order(y) > from) == (from > 0)) {
        v.u++;
    } else {
        v.u--;
    }
    return v.d;
}

double cbrt(double x) {
    if (x == 0.0 || isnan(x) || isinf(x)) {
        return x;
    }
    int neg = x < 0.0;
    double ax = fabs(x);
    int e;
    double m = frexp(ax, &e);
    int r = e % 3;
    if (r < 0) {
        r += 3;
    }
    double y = exp(log(ldexp(m, r)) / 3.0);
    y = ldexp(y, (e - r) / 3);
    for (int i = 0; i < 3; i++) {
        y = y - (y - ax / (y * y)) / 3.0;
    }
    return neg ? -y : y;
}

double exp2(double x) {
    if (isnan(x)) {
        return x;
    }
    if (isinf(x)) {
        return signbit(x) ? 0.0 : x;
    }
    if (x > 1024.0) {
        return overflow_error(0);
    }
    if (x < -1075.0) {
        return 0.0;
    }
    double n = round(x);
    double f = x - n;
    return ldexp(exp(f * LN2), (int)n);
}

#define FMA_SPLIT 134217729.0

static void two_product(double a, double b, double *hi, double *lo) {
    double p = a * b;
    double ca = FMA_SPLIT * a;
    double ah = ca - (ca - a), al = a - ah;
    double callback = FMA_SPLIT * b;
    double bh = callback - (callback - b), bl = b - bh;
    *hi = p;
    *lo = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

static void two_sum(double a, double b, double *hi, double *lo) {
    double s = a + b;
    double z = s - a;
    *hi = s;
    *lo = (a - (s - z)) + (b - z);
}

double fma(double x, double y, double z) {
    if (isnan(x) || isnan(y) || isnan(z) || isinf(x) || isinf(y) ||
        isinf(z) || x == 0.0 || y == 0.0) {
        return x * y + z;
    }
    int ex, ey, ez;
    frexp(x, &ex);
    frexp(y, &ey);
    frexp(z, &ez);
    int scale = 0;
    if (ex + ey > 500 || ex + ey < -500 || ez > 500 || ez < -500) {
        scale = (ex + ey) / 2;
        x = ldexp(x, -scale);
        y = ldexp(y, -scale);
        z = ldexp(z, -2 * scale);
    }
    double ph, pl;
    two_product(x, y, &ph, &pl);
    double sh, sl;
    two_sum(ph, z, &sh, &sl);
    double r = sh + (sl + pl);
    return scale ? ldexp(r, 2 * scale) : r;
}

float fabsf(float x) {
    return (float)fabs((double)x);
}

float floorf(float x) {
    return (float)floor((double)x);
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

float sinf(float x) {
    return (float)sin((double)x);
}

float cosf(float x) {
    return (float)cos((double)x);
}

float tanf(float x) {
    return (float)tan((double)x);
}

float hypotf(float x, float y) {
    return (float)hypot((double)x, (double)y);
}

float sqrtf(float x) {
    return (float)sqrt((double)x);
}

float atanf(float x) {
    return (float)atan((double)x);
}

float roundf(float x) {
    return (float)round((double)x);
}

/* A quiet NaN, which C99 spells as a function taking a payload string
   nothing here has a use for. The payload is ignored on purpose rather
   than by omission: this machine's NaNs carry no diagnostic. */
double nan(const char *tag) {
    (void)tag;
    return __builtin_nan("");
}

float nanf(const char *tag) {
    (void)tag;
    return __builtin_nanf("");
}

float tanhf(float x) {
    return (float)tanh((double)x);
}

float expf(float x) {
    return (float)exp((double)x);
}

float atan2f(float y, float x) {
    return (float)atan2((double)y, (double)x);
}

/* A double carries every float exponent, so scaling in double and rounding
   once is exact wherever the result is a normal float, and rounds correctly
   where it is not. */
float ldexpf(float x, int exponent) {
    return (float)ldexp((double)x, exponent);
}

/* Not exp's treatment of (float)nextafter((double)x, (double)y): the step
   between two adjacent doubles is far below a float's, so rounding it back
   would return x itself for every input. A float's neighbour has to be found
   in a float's own bits. */
float nextafterf(float x, float y) {
    if (isnan(x) || isnan(y)) {
        return x + y;
    }
    if (x == y) {
        return y;
    }
    union { float f; unsigned int u; } v;
    if (x == 0.0f) {
        v.u = 1;
        return signbit(y) ? -v.f : v.f;
    }
    v.f = x;
    int from = float_order(x);
    if ((float_order(y) > from) == (from > 0)) {
        v.u++;
    } else {
        v.u--;
    }
    return v.f;
}

/* M155. The rest of C99 7.12, which V8 asked for and this library had not
   had: the twelve double functions below and, after them, a float variant for
   every function that has one.

   The double ones delegate to the long double family rather than being
   written again, and that is exact rather than merely close: this target's
   long double carries a 64-bit mantissa where a double carries 53, so every
   one of these operations - a remainder, a rounding to integer, an exponent -
   is computed without error and rounded once on the way out. Those
   implementations are M142's, and /bin/mathltest already grades all of them
   against MPFR on the machine.

   lgamma and tgamma used to be the one gap in C99's set here, left because
   nothing in this tree had asked for them. M160: something did. Eigen's
   SpecialFunctionsImpl.h calls lgammaf, tflite includes it, and the
   //cc build stopped in forty-seven translation units on one missing
   declaration. They are below, in double and float; lgammal and tgammal are
   still absent, and their condition is the one math.h states - a program on
   this machine that calls one - which lgammaf is not. */
double nearbyint(double x) {
    return (double)nearbyintl((long double)x);
}

double rint(double x) {
    return (double)rintl((long double)x);
}

long lrint(double x) {
    return lrintl((long double)x);
}

long long llrint(double x) {
    return llrintl((long double)x);
}

double remainder(double x, double y) {
    return (double)remainderl((long double)x, (long double)y);
}

double remquo(double x, double y, int *quotient) {
    return (double)remquol((long double)x, (long double)y, quotient);
}

double logb(double x) {
    return (double)logbl((long double)x);
}

int ilogb(double x) {
    return ilogbl((long double)x);
}

double scalbn(double x, int exponent) {
    return (double)scalbnl((long double)x, exponent);
}

double scalbln(double x, long exponent) {
    return (double)scalblnl((long double)x, exponent);
}

double fdim(double x, double y) {
    return (double)fdiml((long double)x, (long double)y);
}

/* Not nexttowardl rounded down, which would step in the wrong space: the
   answer has to be the next DOUBLE, and a long double y sitting between x and
   it must still move x a whole double ulp. Converting y to double first would
   lose exactly the cases this function exists for. */
double nexttoward(double x, long double y) {
    if (isnan(x)) {
        return x;
    }
    if (isnan(y)) {
        return (double)y;
    }
    if ((long double)x == y) {
        return (double)y;
    }
    return nextafter(x, isless((long double)x, y) ? HUGE_VAL : -HUGE_VAL);
}

/* The float family. Every one of these computes in double and rounds once,
   which is what the sixteen that were already here do - a double carries
   every float exactly and has 29 more mantissa bits to work in, so the only
   error is the single rounding on the way back. */

float acosf(float x) {
    return (float)acos((double)x);
}

float asinf(float x) {
    return (float)asin((double)x);
}

float acoshf(float x) {
    return (float)acosh((double)x);
}

float asinhf(float x) {
    return (float)asinh((double)x);
}

float atanhf(float x) {
    return (float)atanh((double)x);
}

float coshf(float x) {
    return (float)cosh((double)x);
}

float sinhf(float x) {
    return (float)sinh((double)x);
}

float exp2f(float x) {
    return (float)exp2((double)x);
}

/* M160. The gamma function and its logarithm.
 *
 * Lanczos, g = 7 with nine coefficients, which is the approximation whose
 * error over the right half plane is below one part in 10^15 - chosen over
 * the six-coefficient set every textbook prints, which is good to ten digits
 * and would have needed a tolerance in cases.tsv with an excuse beside it.
 *
 * Three things here are not the textbook version, and each was a failure
 * against the host's libm before it was a decision.
 *
 * The left half plane is Euler's reflection, G(x)G(1-x) = pi/sin(pi x), and
 * near a zero of lgamma - there is one between every pair of poles, where
 * |G| passes through 1 - the two terms cancel to nothing. Graded relatively,
 * a double evaluation was out by 6.7e-13 at x = -2.4559 on a value of
 * 0.00165, which is 1.1e-15 ABSOLUTE and as good as a double can do. So the
 * reflection is computed in long double and rounded once, which is M142's
 * trick and buys the eleven extra mantissa bits the cancellation eats.
 *
 * The accuracy of the reflection is the accuracy of sin(pi x), and handing
 * sin() the product pi*x is what loses it: for x = -20.3 the product is
 * already rounded, and the sine of a rounded argument near a multiple of pi
 * is wrong in its leading digits. The integer part is removed FIRST, where
 * the subtraction is exact, and only the fraction is multiplied by pi - the
 * same lesson M142 learned from the x87's fsin.
 *
 * And near zero neither the reflection nor Lanczos is right: G(x) = G(x+1)/x
 * is, exactly, and it is what gives lgamma(DBL_TRUE_MIN) = 744.44 instead of
 * an infinity out of a sine that underflowed.
 */
static const long double gamma_lanczos_g = 7.0L;

static const long double gamma_lanczos[9] = {
    0.99999999999980993L,
    676.5203681218851L,
    -1259.1392167224028L,
    771.32342877765313L,
    -176.61502916214059L,
    12.507343278686905L,
    -0.13857109526572012L,
    9.9843695780195716e-6L,
    1.5056327351493116e-7L,
};

/* sqrt(2*pi) and pi, to more digits than a double holds. */
static const long double gamma_sqrt_two_pi = 2.50662827463100050242E0L;
static const long double gamma_pi = 3.14159265358979323846L;

static long double gamma_lanczos_sum(long double z) {
    long double sum = gamma_lanczos[0];
    for (int i = 1; i < 9; i++) {
        sum += gamma_lanczos[i] / (z + (long double)i);
    }
    return sum;
}

/* sin(pi * x), with the integer part taken off before anything is multiplied
   by pi. x - floorl(x) is exact, so the only rounding left is the one
   multiplication and the sine itself. */
static long double sin_pi_l(long double x) {
    long double y = fabsl(x);
    if (y >= 18446744073709551616.0L) {
        /* Every value this large is an even integer. */
        return x < 0.0L ? -0.0L : 0.0L;
    }
    long double whole = floorl(y);
    long double fraction = y - whole;
    long double s;
    if (fraction == 0.0L) {
        s = 0.0L;
    } else if (fraction <= 0.25L) {
        s = sinl(gamma_pi * fraction);
    } else if (fraction < 0.75L) {
        s = cosl(gamma_pi * (0.5L - fraction));
    } else {
        s = sinl(gamma_pi * (1.0L - fraction));
    }
    if (fmodl(whole, 2.0L) != 0.0L) {
        s = -s;
    }
    return x < 0.0L ? -s : s;
}

/* log|G(x)| for x >= 0.5, which is the half Lanczos is written for. */
static long double lgamma_right(long double x) {
    long double z = x - 1.0L;
    long double t = z + gamma_lanczos_g + 0.5L;
    return logl(gamma_sqrt_two_pi) + (z + 0.5L) * logl(t) - t +
           logl(gamma_lanczos_sum(z));
}

/* The largest x for which G(x) is finite in a double. G(171.7) overflows;
   G(171.6) does not. */
static const double gamma_overflow_at = 171.61447887182298;

int signgam;

double tgamma(double x) {
    if (isnan(x)) {
        return x;
    }
    if (isinf(x)) {
        if (x > 0.0) {
            return x;
        }
        return domain_error();
    }
    if (x == 0.0) {
        /* A pole, and the sign of the zero decides which infinity. */
        return pole_error(1.0 / x < 0.0);
    }
    if (x < 0.0 && x == floor(x)) {
        return domain_error();
    }
    if (x > gamma_overflow_at) {
        return overflow_error(0);
    }

    /* Near zero, G(x) = G(x+1)/x exactly, which is both more accurate than
       anything else here and the only form that survives a subnormal. */
    if (fabs(x) < 0.5) {
        return tgamma(x + 1.0) / x;
    }

    if (x < 0.0) {
        long double s = sin_pi_l((long double)x);
        if (s == 0.0L) {
            return overflow_error(0);
        }
        /* G(x) = pi / (sin(pi x) * G(1-x)), in long double so that the
           product below does not round twice. */
        long double magnitude = expl(lgamma_right(1.0L - (long double)x));
        return (double)(gamma_pi / (s * magnitude));
    }

    long double z = (long double)x - 1.0L;
    long double t = z + gamma_lanczos_g + 0.5L;
    long double sum = gamma_lanczos_sum(z);
    /* t^(z+0.5) overflows a double long before G does, and the x87's range
       is wide enough that in long double it does not. */
    long double value =
        gamma_sqrt_two_pi * sum * powl(t, z + 0.5L) * expl(-t);
    return (double)value;
}

double lgamma(double x) {
    signgam = 1;
    if (isnan(x)) {
        return x;
    }
    if (isinf(x)) {
        return INFINITY;
    }
    if (x == 0.0 || (x < 0.0 && x == floor(x))) {
        return pole_error(0);
    }

    /* G(x) = G(x+1)/x near zero, for the reason tgamma gives. */
    if (fabs(x) < 0.5) {
        double result = lgamma(x + 1.0) - log(fabs(x));
        signgam = x < 0.0 ? -1 : 1;
        return result;
    }

    if (x < 0.0) {
        long double s = sin_pi_l((long double)x);
        signgam = s < 0.0L ? -1 : 1;
        /* log(pi/|sin(pi x)|) - log|G(1-x)|, in long double and rounded once.
           The two terms cancel to nothing at each zero of lgamma, and this is
           the eleven bits that buys. */
        long double value = logl(gamma_pi / fabsl(s)) -
                            lgamma_right(1.0L - (long double)x);
        return (double)value;
    }

    return (double)lgamma_right((long double)x);
}

float tgammaf(float x) {
    return (float)tgamma((double)x);
}

float lgammaf(float x) {
    return (float)lgamma((double)x);
}

float expm1f(float x) {
    return (float)expm1((double)x);
}

float logf(float x) {
    return (float)log((double)x);
}

float log10f(float x) {
    return (float)log10((double)x);
}

float log1pf(float x) {
    return (float)log1p((double)x);
}

float log2f(float x) {
    return (float)log2((double)x);
}

float logbf(float x) {
    return (float)logb((double)x);
}

float cbrtf(float x) {
    return (float)cbrt((double)x);
}

float erff(float x) {
    return (float)erf((double)x);
}

float erfcf(float x) {
    return (float)erfc((double)x);
}

float nearbyintf(float x) {
    return (float)nearbyint((double)x);
}

float rintf(float x) {
    return (float)rint((double)x);
}

float truncf(float x) {
    return (float)trunc((double)x);
}

float powf(float x, float y) {
    return (float)pow((double)x, (double)y);
}

float fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}

float remainderf(float x, float y) {
    return (float)remainder((double)x, (double)y);
}

float copysignf(float x, float y) {
    return (float)copysign((double)x, (double)y);
}

float fdimf(float x, float y) {
    return (float)fdim((double)x, (double)y);
}

float fmaxf(float x, float y) {
    return (float)fmax((double)x, (double)y);
}

float fminf(float x, float y) {
    return (float)fmin((double)x, (double)y);
}

float fmaf(float x, float y, float z) {
    return (float)fma((double)x, (double)y, (double)z);
}

int ilogbf(float x) {
    return ilogb((double)x);
}

long lrintf(float x) {
    return lrint((double)x);
}

long long llrintf(float x) {
    return llrint((double)x);
}

float scalbnf(float x, int exponent) {
    return (float)scalbn((double)x, exponent);
}

float scalblnf(float x, long exponent) {
    return (float)scalbln((double)x, exponent);
}

float frexpf(float x, int *exponent) {
    return (float)frexp((double)x, exponent);
}

float modff(float x, float *ipart) {
    double whole;
    double fraction = modf((double)x, &whole);
    *ipart = (float)whole;
    return (float)fraction;
}

float remquof(float x, float y, int *quotient) {
    return (float)remquo((double)x, (double)y, quotient);
}

float nexttowardf(float x, long double y) {
    if (isnan(x)) {
        return x;
    }
    if (isnan(y)) {
        return (float)y;
    }
    if ((long double)x == y) {
        return (float)y;
    }
    return nextafterf(x, isless((long double)x, y) ? HUGE_VALF : -HUGE_VALF);
}
