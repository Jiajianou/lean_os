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
   reports arguments. */
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

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double sqrt(double x) {
    if (x < 0.0) {
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
    return (x < 0.0 && t != x) ? t - 1.0 : t;
}

double ceil(double x) {
    if (!isfinite(x) || fabs(x) >= TWO_POW_52) {
        return x;
    }
    double t = (double)(long long)x;
    return (x > 0.0 && t != x) ? t + 1.0 : t;
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
    if (isnan(x)) {
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
    if (isnan(x)) {
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

double pow(double x, double y) {
    if (y == 0.0) {
        return 1.0;
    }
    if (x == 0.0) {
        return y > 0.0 ? 0.0 : pole_error(0);
    }
    if (x < 0.0) {
        double ry = (double)(long long)y;
        if (ry != y) {
            return domain_error();
        }
        double mag = exp(y * log(-x));
        return ((long long)y & 1) ? -mag : mag;
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
    return x < 0 ? ceil(x) : floor(x);
}

double modf(double x, double *ipart) {
    double i = trunc(x);
    if (ipart) {
        *ipart = i;
    }
    return x - i;
}

double round(double x) {
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

long lround(double x) {
    double r = round(x);
    if (r >= (double)LONG_MIN && r < (double)LONG_MAX) {
        return (long)r;
    }
    return LONG_MIN;
}

long lroundf(float x) {
    return lround((double)x);
}

long long llround(double x) {
    double r = round(x);
    if (r >= (double)LLONG_MIN && r < (double)LLONG_MAX) {
        return (long long)r;
    }
    return LLONG_MIN;
}

long long llroundf(float x) {
    return llround((double)x);
}

double copysign(double x, double y) {
    double m = x < 0 ? -x : x;
    return signbit(y) ? -m : m;
}

double hypot(double x, double y) {
    double ax = x < 0 ? -x : x;
    double ay = y < 0 ? -y : y;
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
    if (isnan(x) || isinf(x)) {
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
    if (isnan(x)) {
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
    return a > b ? a : b;
}

double fmin(double a, double b) {
    if (isnan(a)) {
        return b;
    }
    if (isnan(b)) {
        return a;
    }
    return a < b ? a : b;
}

double expm1(double x) {
    if (isnan(x)) {
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
        return y > 0.0 ? v.d : -v.d;
    }
    v.d = x;
    if ((y > x) == (x > 0.0)) {
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
        return y > 0.0f ? v.f : -v.f;
    }
    v.f = x;
    if ((y > x) == (x > 0.0f)) {
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

   lgamma and tgamma are not here, and are not in the long double family
   either. That is the one gap in C99's set this library has, and it is a gap
   rather than an oversight: nothing in this tree has asked for them, and a
   Lanczos approximation written to fill a table is the kind of code M65's
   rule is about. */
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
    return nextafter(x, (long double)x < y ? HUGE_VAL : -HUGE_VAL);
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
    return nextafterf(x, (long double)x < y ? HUGE_VALF : -HUGE_VALF);
}
