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
#if defined(__x86_64__)
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    /* M99. This branch never ships: the only machine that compiles this
     * file for anything but x86-64 is the host running
     * tools/math-test.sh, which is an arm64 Mac here. `__builtin_sqrt`
     * rather than a Newton iteration written for the occasion, because
     * on that host it is one `fsqrt` instruction and a hand-written
     * iteration would be a second thing to be wrong.
     *
     * The honest consequence, and it is recorded in
     * tests/math/cases.tsv beside the sqrt row rather than only here:
     * on the host, the `sqrt` row grades the host's own instruction
     * against the host's own libm. Every other row is graded for real,
     * including all the ones that CALL sqrt. */
    return __builtin_sqrt(x);
#endif
}

/* 2^52. At and above it every double is already a whole number, and -
 * the part that matters - a `(long long)` cast of anything at or above
 * 2^63 is undefined behaviour rather than a large answer. M99 added the
 * guard when tools/math-test.sh started sweeping past 1e18; the failure
 * before it was not an inaccuracy, it was floor(1e300) returning
 * whatever the cvttsd2si instruction does with a value it cannot hold,
 * which on x86-64 is the "integer indefinite" -2^63. */
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

/* ---- M99: fmod, rewritten, and what the first differential run found -
 *
 * This was `x - (long long)(x / y) * y`, and tools/math-test.sh's very
 * first run failed it: fmod(-96.666..., 4.44e-15) came back as
 * +7.1e-15 - the wrong sign, and larger than the modulus, which is two
 * things fmod is defined never to be.
 *
 * The reason is that the quotient is not the answer. x/y there is
 * -2.2e16; rounding that to an integer and multiplying it back by y
 * reconstructs a number that agrees with x in about two significant
 * digits, and the subtraction keeps only the disagreement. The old form
 * is exact only while x/y is small enough that the rounding loses
 * nothing, which is a range nobody documented and nothing checked.
 *
 * What is written now is the shift-and-subtract every libm uses, and it
 * is exact rather than accurate. Two facts about binary floating point
 * carry it: scaling by a power of two is exact, and subtracting two
 * values within a factor of two of each other is exact (Sterbenz). The
 * loop maintains d <= r < 2d at every step, so every subtraction it
 * performs is one of those - and the result is the true remainder, bit
 * for bit, for every pair of finite doubles.
 */
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
    double d = ldexp(ay, ex - ey); /* the same magnitude as ax, exactly */
    for (int k = ex - ey; k >= 0; k--) {
        if (r >= d) {
            r -= d;
        }
        d *= 0.5;
    }
    return x < 0.0 ? -r : r;
}

/* ---- trigonometry ----------------------------------------------------
 *
 * Range reduction first, then a Taylor series on the reduced argument.
 * The reduction is what makes the series usable: sin's Taylor expansion
 * is excellent near zero and useless at x = 1000, and a library that
 * skipped this step would be accurate exactly where nobody calls it. */
#define TWO_PI 6.283185307179586476925286766559
#define M_PI_4_ 0.78539816339744830962 /* M99 - atan2 at infinity */

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

/* ---- M99: the range this reduction is valid over, stated -------------
 *
 * `quadrant` divides by pi/2 and rounds to a long long. That is exact
 * while the quotient's integer part still fits in the mantissa of a
 * double - up to about 2^52 - and past that the cast is undefined
 * behaviour rather than a large number. A correct answer out there needs
 * Payne-Hanek reduction against a thousand bits of pi, which is a real
 * piece of work and is not this milestone's.
 *
 * So the range is declared and the outside of it REFUSES. NaN is a worse
 * answer than 0.00496 and a much better one than a plausible number
 * computed from a garbage quadrant: a program that asks for sin(1e308)
 * finds out that this libm will not answer, which is what math.h's
 * standing claim - "not a replacement for a correctly-rounded libm" -
 * means in this one place. tests/math/cases.tsv records it as a
 * divergence from the host with the reason attached, so every run of the
 * harness prints it rather than hiding it. */
#define TRIG_MAX_ARG 4503599627370496.0 /* 2^52 */

/* How many quadrants of pi/2 are in x, and what is left over. */
static int quadrant(double x, double *r) {
    double nd = x / M_PI_2;
    long long n = (long long)(nd >= 0.0 ? nd + 0.5 : nd - 0.5);
    double dn = (double)n;
    *r = ((x - dn * PIO2_A) - dn * PIO2_B) - dn * PIO2_C;
    return (int)(((n % 4) + 4) % 4);
}

double sin(double x) {
    /* M99: NaN for a non-finite argument (which C99 specifies) and for
     * one past TRIG_MAX_ARG (which this libm declares - see above). */
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARG) {
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
    /* M99: NaN for a non-finite argument (which C99 specifies) and for
     * one past TRIG_MAX_ARG (which this libm declares - see above). */
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARG) {
        return NAN;
    }
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
    if (!isfinite(x) || fabs(x) > TRIG_MAX_ARG) {
        return NAN;
    }
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
    /* M99: both infinite, which is the case the division cannot answer -
     * inf/inf is NaN, and the answer is one of the four diagonals. C99
     * specifies each of them. */
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

/* ---- exponential and logarithm --------------------------------------- */

#define LN2 0.69314718055994530942

double exp(double x) {
    /* M99: NaN first. Without this the range reduction below computes
     * `(long long)(nan + 0.5)`, which is undefined behaviour producing
     * an arbitrary 64-bit integer, and the squaring loop then runs that
     * many times - a hang rather than a wrong answer. */
    if (isnan(x)) {
        return x;
    }
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

/* ---- M99: and what happens at infinity -------------------------------
 *
 * This function had no guard for a non-finite argument, and the scaling
 * loop below is `while (x > sqrt(2)) x *= 0.5;` - **infinity halved is
 * infinity**, so log(INFINITY) did not return a wrong answer, it did not
 * return. CPython's own test suite is what found it: test_math's
 * testAcosh calls acosh(INF), acosh calls log for a large argument, and
 * the module's five-minute watchdog fired with a traceback whose top
 * frame was `testAcosh`.
 *
 * tests/math/cases.tsv now has a special-values section for every
 * function, and it is the more useful half of this fix: a libm harness
 * that only sweeps finite ranges never asks what happens at infinity,
 * and infinity is exactly where a range-reduction loop stops
 * terminating. */
double log(double x) {
    if (isnan(x) || (isinf(x) && x > 0.0)) {
        return x; /* log(+inf) is +inf; log(nan) is nan */
    }
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

/* ---- M80 groundwork ----------------------------------------------------
 *
 * The decomposition and classification helpers, in the same style as
 * everything above: plain C over doubles, no lookup tables, and no
 * pretence at last-bit accuracy. What each one promises is stated where
 * it differs from the textbook.
 */

/* Exact, and deliberately so: frexp is the one function here that has to
 * be, because it is how a program takes a double apart to print it or to
 * convert it to an integer type, and an approximate answer there is a
 * wrong digit rather than a rounding error. Done by scaling in powers of
 * two, which is exact in binary floating point at every step. */
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
    /* Repeated multiplication rather than a bit-twiddle on the exponent
     * field: multiplying by 2 is exact, so this is exact too, and it
     * handles overflow and subnormals by simply producing what the
     * hardware produces.
     *
     * M99: bounded, because "repeated" is the whole implementation and
     * `exp` is an int. ldexp(1.0, 2000000000) is two billion
     * multiplications of an infinity, which is not a slow answer, it is
     * no answer. A double's exponent spans about [-1074, 1024], so
     * anything past that is already the saturated result. */
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
    /* Away from zero on a tie, which is what C's round() specifies -
     * and differs from the nearest-even a bare cast or the hardware's
     * default rounding would give.
     *
     * M99: written on the *fractional part* rather than as
     * floor(x + 0.5), which is the textbook form and is wrong for
     * exactly one input: 0.49999999999999994, the double just below a
     * half. Adding 0.5 to it rounds up to 1.0 before floor ever sees it,
     * so the old form answered 1 where the answer is 0. */
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

double copysign(double x, double y) {
    double m = x < 0 ? -x : x;
    return signbit(y) ? -m : m;
}

double hypot(double x, double y) {
    /* Scaled, so that a large x does not overflow on the way to a
     * perfectly representable answer - the whole reason hypot exists
     * rather than sqrt(x*x + y*y). */
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
    return log(x) * 1.4426950408889634074; /* 1 / ln 2 */
}

/* M99: the three hyperbolics grew guards for the same reason log did.
 * Each is written in terms of exp, and exp saturates - so at large |x|
 * the expressions below become inf-inf or inf/inf, which is NaN, when
 * the answers are ±inf, +inf and ±1. */
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
        return HUGE_VAL; /* cosh is even: +inf either way */
    }
    double e = exp(x);
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x) {
    if (isnan(x)) {
        return x;
    }
    /* Saturates well before exp does: tanh(20) differs from 1 by 4e-18,
     * which is below the last bit of a double. */
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

/* ---- M99: the C99 set CPython refuses to configure without ------------
 *
 * `configure` for CPython 3.12 checks `acosh asinh atanh erf erfc expm1
 * log1p log2` in one loop and calls a missing one a fatal error:
 * "Python requires C99 compatible libm". log2 was here since M63; the
 * other seven are below, written for the same reason as everything above
 * them - there is no libm to fall back on.
 *
 * Each one is here because a naive expression of it loses most of its
 * significant digits somewhere in its range, which is the whole reason
 * C99 named them separately in the first place. `exp(x) - 1` at
 * x = 1e-9 is a subtraction of two numbers that agree to nine places;
 * `log(1 + x)` at the same x has already lost them in the addition. The
 * ranges below are chosen at the point where the direct form stops
 * cancelling, and tools/math-test.sh grades every one of them against
 * the host's own libm rather than against a table written here.
 */

/* e^x - 1. The Maclaurin series where the direct form cancels; exp()
 * itself where it does not. |x| < 1 keeps the alternating case's terms
 * shrinking by at least a factor of the index, so 25 of them reach the
 * bottom of a double (1/25! is 6e-26). */
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

/* log(1 + x), through the artanh form for small x:
 *
 *   log(1 + x) = 2 * artanh(s),  s = x / (2 + x)
 *
 * which is the classical way to get this without cancellation. |x| < 0.5
 * bounds |s| at 1/3, so s^2 <= 1/9 and the odd series converges by a
 * decimal digit every term and a bit. */
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

/* asinh(x) = log(x + sqrt(x^2 + 1)), except that for small x the sqrt is
 * 1 and the log's argument is 1 + something tiny - so the log1p form is
 * used there, and for large x the x^2 would overflow long before asinh
 * does, so the asymptote log(2x) is used instead. */
double asinh(double x) {
    if (isnan(x) || isinf(x) || x == 0.0) {
        return x;
    }
    double ax = fabs(x);
    double r;
    if (ax > 1e8) {
        /* sqrt(x^2+1) == x to every bit a double has by here. */
        r = log(ax) + 0.69314718055994530942; /* + ln 2 */
    } else {
        r = log1p(ax + ax * ax / (1.0 + sqrt(ax * ax + 1.0)));
    }
    return x < 0.0 ? -r : r;
}

/* acosh(x) = log(x + sqrt(x^2 - 1)), x >= 1. Near 1 the sqrt goes to
 * zero and the log's argument to 1, so it is written in terms of
 * t = x - 1, which is the quantity the caller still has all the digits
 * of. */
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

/* atanh(x) = 0.5 * log((1 + x) / (1 - x)), written as log1p of the
 * quantity that is small when x is small. */
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

/* The error function, in two pieces with the join at |x| = 2.
 *
 * Below it, the all-positive series
 *
 *   erf(x) = (2/sqrt(pi)) e^(-x^2) SUM x (2x^2)^n / (1.3.5...(2n+1))
 *
 * which has no cancellation anywhere in it - unlike the alternating
 * Taylor series for erf, whose terms at x = 2 reach 30 before they
 * shrink and take four digits with them.
 *
 * Above it, the continued fraction for erfc
 *
 *   erfc(x) = e^(-x^2)/sqrt(pi) * 1/(x + (1/2)/(x + 1/(x + (3/2)/(x + ...))))
 *
 * evaluated backwards from a fixed depth, which is what makes erfc(20)
 * a number rather than 1 - 1.
 */
#define M_2_SQRTPI_ 1.12837916709551257390 /* 2/sqrt(pi) */
#define M_1_SQRTPI_ 0.56418958354775628695 /* 1/sqrt(pi) */

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

/* erfc for x >= 2, where e^(-x^2) is already 1.8e-2 and falling. Forty
 * levels is well past convergence at x = 2 and the fraction only gets
 * shorter as x grows. */
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
    /* erfc(6) is 2.2e-17, which is below the last bit of 1.0. */
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
    /* e^(-27^2) is below the smallest subnormal double. */
    if (x > 27.0) {
        return 0.0;
    }
    return erfc_cf(x);
}

/* The next representable double from x towards y.
 *
 * Bit arithmetic rather than arithmetic, because that is what the
 * function means: doubles of the same sign are ordered by their bit
 * patterns read as integers, so "the next one" is +/- 1 on that integer.
 * Here for M99: CPython's math.nextafter and math.ulp call it directly
 * rather than through a HAVE_ guard, so it is not optional the way
 * lgamma is. */
double nextafter(double x, double y) {
    if (isnan(x) || isnan(y)) {
        return x + y;
    }
    if (x == y) {
        return y;
    }
    union { double d; unsigned long long u; } v;
    if (x == 0.0) {
        /* The smallest subnormal, signed towards y. Zero has two bit
         * patterns and neither of them is adjacent to anything. */
        v.u = 1;
        return y > 0.0 ? v.d : -v.d;
    }
    v.d = x;
    /* Away from zero when x and y point the same way from it, towards
     * zero otherwise - which is the only place the sign of x enters. */
    if ((y > x) == (x > 0.0)) {
        v.u++;
    } else {
        v.u--;
    }
    return v.d;
}

/* ---- M99: three more the interpreter's own math module needs ---------
 *
 * Not from a standard's list - from Modules/mathmodule.c, which calls
 * all three by name and none of them behind a HAVE_ guard.
 */

/* The real cube root, which is not pow(x, 1.0/3.0): 1/3 is not a double,
 * so that form is wrong in the last places for every input and is a
 * domain error for every negative one. Seeded through frexp so the
 * exponent is divided exactly and only the mantissa goes through log and
 * exp, then three Newton steps on y^3 = x, which square the number of
 * correct digits each time. */
double cbrt(double x) {
    if (x == 0.0 || isnan(x) || isinf(x)) {
        return x;
    }
    int neg = x < 0.0;
    double ax = fabs(x);
    int e;
    double m = frexp(ax, &e);
    /* Split the exponent into a multiple of three and a remainder, so
     * the part that goes through exp/log is bounded and the rest is an
     * exact scaling. */
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

/* 2^x. exp(x * ln2) would do, except that it is not exact at the
 * integers - and exp2(10) not being 1024 exactly is the kind of thing
 * that shows up as an off-by-one-ulp in somebody else's test suite. The
 * integer part is separated first and applied with ldexp, which is
 * exact; only the fractional half, bounded by 1/2, goes through exp. */
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

/* ---- fma, and why it is written out rather than left to the hardware -
 *
 * x86-64's base instruction set has no fused multiply-add; FMA3 arrived
 * in 2013 and is not something this project assumes, because M90's rule
 * about the machine applies to the ISA too. `__builtin_fma` on a target
 * without it emits a CALL to fma, which here would be a call to this
 * function - infinite recursion, and one of the few ways to write a
 * stack overflow with no loop in it.
 *
 * So it is Dekker's algorithm, and what makes it a fused multiply-add
 * rather than a multiply and an add is that the product is kept
 * EXACTLY, as two doubles, before z is added. The splitting constant is
 * 2^27 + 1: multiplying by it and subtracting recovers the top 26 bits
 * of a mantissa, so each factor becomes a sum of two 26-bit halves whose
 * four pairwise products are each exact.
 *
 * The scaling either side of it is not decoration. x * SPLIT overflows
 * for |x| above about 1.3e292, and an fma that returned infinity for
 * arguments whose fused result is finite would be worse than no fma at
 * all - that is precisely the case fma exists to get right.
 *
 * CPython is why this is here: Modules/mathmodule.c's dl_mul computes
 * `fma(x, y, -z)` to recover the error of a product, which is the one
 * use of fma that has to be exact rather than accurate. It is - the
 * subtraction cancels the whole product and what comes back is the error
 * term, which is what this function computed on the way.
 */
#define FMA_SPLIT 134217729.0 /* 2^27 + 1 */

static void two_product(double a, double b, double *hi, double *lo) {
    double p = a * b;
    double ca = FMA_SPLIT * a;
    double ah = ca - (ca - a), al = a - ah;
    double cb = FMA_SPLIT * b;
    double bh = cb - (cb - b), bl = b - bh;
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
    /* Bring both factors into the range where the 26-bit split cannot
     * overflow, remembering the scaling to undo at the end. Powers of
     * two, so every step of it is exact. */
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
