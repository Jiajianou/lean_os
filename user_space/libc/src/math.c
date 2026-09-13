#include <limits.h>
#include <math.h>

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double sqrt(double x) {
    if (x < 0.0) {
        return NAN;
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
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) {
        return NAN;
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
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARGUMENT) {
        return NAN;
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
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARGUMENT) {
        return NAN;
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
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARGUMENT) {
        return NAN;
    }
    double c = cos(x);
    if (c == 0.0) {
        return HUGE_VAL;
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
        return NAN;
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
        return HUGE_VAL;
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
        return NAN;
    }
    if (x == 0.0) {
        return -HUGE_VAL;
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
        return y > 0.0 ? 0.0 : HUGE_VAL;
    }
    if (x < 0.0) {
        double ry = (double)(long long)y;
        if (ry != y) {
            return NAN;
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
        return NAN;
    }
    if (x == -1.0) {
        return -HUGE_VAL;
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
        return NAN;
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
        return NAN;
    }
    if (ax == 1.0) {
        return x > 0.0 ? HUGE_VAL : -HUGE_VAL;
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
        return HUGE_VAL;
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
