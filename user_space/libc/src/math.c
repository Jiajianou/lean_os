/* M63. See math.h for the accuracy this claims and for why the function
 * list is the one it is.
 *
 * Two things shape everything here. There is no libm to fall back on, so
 * each of these is written out; and this OS now has SSE2, so `sqrt` is
 * one instruction rather than a Newton iteration - which matters because
 * everything else leans on it. */
#include <math.h>

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double sqrt(double x) {
    if (x < 0.0) {
        return NAN;
    }
    /* SSE2's own square root - correctly rounded, and the reason M63's
     * FPU work had to land before this file could exist at all. */
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double floor(double x) {
    double t = (double)(long long)x;
    return (x < 0.0 && t != x) ? t - 1.0 : t;
}

double ceil(double x) {
    double t = (double)(long long)x;
    return (x > 0.0 && t != x) ? t + 1.0 : t;
}

double fmod(double x, double y) {
    if (y == 0.0) {
        return NAN;
    }
    double q = x / y;
    double t = (double)(long long)q;
    return x - t * y;
}

/* ---- trigonometry ----------------------------------------------------
 *
 * Range reduction first, then a Taylor series on the reduced argument.
 * The reduction is what makes the series usable: sin's Taylor expansion
 * is excellent near zero and useless at x = 1000, and a library that
 * skipped this step would be accurate exactly where nobody calls it. */
#define TWO_PI 6.283185307179586476925286766559

/* Cody-Waite: pi/2 split so that n * PIO2_A is *exact*.
 *
 * Two separate problems live in "reduce the argument", and this file got
 * to fail its own self-test on each of them in turn.
 *
 * The first is precision. `x - n * (pi/2)` loses a bit for every power of
 * two in n, because the constant is only the nearest double to pi/2 and
 * the product rounds. 1.5703125 is 1.1001001 in binary - seven
 * significant bits - so multiplying it by any integer below about 2^45 is
 * exact, and the rest of pi/2 is carried in two more pieces subtracted
 * afterwards.
 *
 * The second is *range*, and it is the one that actually bit: reducing to
 * [-pi, pi] and then running a Taylor series is not enough, because the
 * series' error grows with the reduced argument and at |r| near pi an
 * eight-term cosine is only good to about 5e-8. cos(-40) is a perfectly
 * ordinary argument and that is exactly where it failed. Reducing to
 * *quadrants* puts |r| below pi/4, where the same eight terms are good to
 * 1e-16 - the cost is one switch, and the sine and cosine kernels are
 * shared between sin() and cos() because at that point they are the same
 * two functions with the quadrant table rotated by one. */
#define PIO2_A 1.5703125
#define PIO2_B 4.83826794896619256404e-04
#define PIO2_C -2.50827880633416613471e-20

/* |r| <= pi/4 for both: eight terms, and the next one is below 1e-16. */
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

/* How many quadrants of pi/2 are in x, and what is left over. */
static int quadrant(double x, double *r) {
    double nd = x / M_PI_2;
    long long n = (long long)(nd >= 0.0 ? nd + 0.5 : nd - 0.5);
    double dn = (double)n;
    *r = ((x - dn * PIO2_A) - dn * PIO2_B) - dn * PIO2_C;
    return (int)(((n % 4) + 4) % 4);
}

double sin(double x) {
    double r;
    switch (quadrant(x, &r)) {
    case 0:  return kernel_sin(r);
    case 1:  return kernel_cos(r);
    case 2:  return -kernel_sin(r);
    default: return -kernel_cos(r);
    }
}

double cos(double x) {
    double r;
    /* The same table, rotated one quadrant - which is all the difference
     * between these two functions is. */
    switch (quadrant(x, &r)) {
    case 0:  return kernel_cos(r);
    case 1:  return -kernel_sin(r);
    case 2:  return -kernel_cos(r);
    default: return kernel_sin(r);
    }
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) {
        return HUGE_VAL;
    }
    return sin(x) / c;
}

double atan(double x) {
    /* Two identities, because the series for atan converges usably only
     * on |x| <= 1: atan(-x) = -atan(x) folds the sign away, and
     * atan(x) = pi/2 - atan(1/x) folds everything above 1 back inside.
     * A further halving via atan(x) = 2*atan(x / (1 + sqrt(1 + x^2)))
     * pulls the argument under 0.42, where fifteen terms are plenty. */
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

/* ---- exponential and logarithm --------------------------------------- */

#define LN2 0.69314718055994530942

double exp(double x) {
    if (x > 709.0) {
        return HUGE_VAL; /* past what a double holds */
    }
    if (x < -745.0) {
        return 0.0;
    }
    /* exp(x) = 2^k * exp(r), with r in [-ln2/2, ln2/2] where the Taylor
     * series converges in a dozen terms. The 2^k is built by squaring
     * rather than by touching the exponent bits, which keeps this
     * readable and costs six multiplies. */
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
    if (x < 0.0) {
        return NAN;
    }
    if (x == 0.0) {
        return -HUGE_VAL;
    }
    /* Scale into [sqrt(0.5), sqrt(2)] by halving or doubling, counting
     * the steps - then atanh's series, which converges far faster on
     * that interval than log's own does. */
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
        /* Only an integer exponent is defined for a negative base, and
         * the sign of the result is the parity of that exponent -
         * anything else is a complex number this returns NAN for rather
         * than guessing at. */
        double ry = (double)(long long)y;
        if (ry != y) {
            return NAN;
        }
        double mag = exp(y * log(-x));
        return ((long long)y & 1) ? -mag : mag;
    }
    return exp(y * log(x));
}
