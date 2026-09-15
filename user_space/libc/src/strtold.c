#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "decimal_powers.h"

/* Before M145 this function was one line:
 *
 *     return (long double)strtod(s, end);
 *
 * which is the trap M142 named when it refused to write the long double
 * math functions that way - a precision claim the compiler lets through.
 * "1.00000000000000000011" has an answer in this format and does not have
 * one in a double, and that version returned the double's.
 *
 * This one accumulates the digits into a 64-bit integer, which is exact for
 * up to nineteen of them, and scales by the tables in decimal_powers.c. The
 * best case is one multiply by an exactly representable power of ten, which
 * rounds once: correctly rounded. The general case is two multiplies and a
 * table entry that is itself half an ulp out, which is where the claim of
 * two units in the last place comes from. tests/strtold grades both against
 * the compiler's own decimal conversion, which is MPFR.
 */

#define DIGIT_LIMIT 19
#define EXPONENT_LIMIT 30000

static long double scale_by_power_of_ten(long double value, int exponent) {
    if (value == 0.0L) {
        return value;
    }
    int negative = exponent < 0;
    int magnitude = negative ? -exponent : exponent;
    if (magnitude > EXPONENT_LIMIT) {
        errno = ERANGE;
        return negative ? 0.0L : copysignl((long double)INFINITY, value);
    }

    int quotient = magnitude / DECIMAL_POWER_STRIDE;
    int remainder = magnitude % DECIMAL_POWER_STRIDE;
    if (quotient >= DECIMAL_POWER_BIG_COUNT) {
        errno = ERANGE;
        return negative ? 0.0L : copysignl((long double)INFINITY, value);
    }

    long double big = DECIMAL_POWER_BIG[quotient];
    long double small = DECIMAL_POWER_EXACT[remainder];
    if (negative) {
        /* Dividing rather than multiplying by a reciprocal: a reciprocal
           would round twice, and the second rounding is the one that would
           not be visible in the table. */
        value = value / big;
        value = value / small;
    } else {
        value = value * small;
        value = value * big;
    }
    if (isinf(value) || value == 0.0L) {
        errno = ERANGE;
    }
    return value;
}

static int match_word(const char *s, const char *word) {
    size_t i = 0;
    while (word[i]) {
        if (tolower((unsigned char)s[i]) != word[i]) {
            return 0;
        }
        i++;
    }
    return (int)i;
}

long double strtold(const char *text, char **end) {
    const char *p = text;
    if (end) {
        *end = (char *)text;
    }
    while (isspace((unsigned char)*p)) {
        p++;
    }

    int negative = 0;
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    int length = match_word(p, "infinity");
    if (!length) {
        length = match_word(p, "inf");
    }
    if (length) {
        p += length;
        if (end) {
            *end = (char *)p;
        }
        return negative ? -(long double)INFINITY : (long double)INFINITY;
    }
    length = match_word(p, "nan");
    if (length) {
        p += length;
        /* The optional n-char-sequence in parentheses, which this library
           has no use for but has to consume so that end is right. */
        if (*p == '(') {
            const char *closing = p + 1;
            while (*closing && *closing != ')') {
                closing++;
            }
            if (*closing == ')') {
                p = closing + 1;
            }
        }
        if (end) {
            *end = (char *)p;
        }
        return (long double)NAN;
    }

    /* The mantissa, as an integer, with the decimal point's position kept
       separately - so nothing is rounded until the scaling at the end. */
    unsigned long long mantissa = 0;
    int digits = 0;
    int dropped = 0;
    int seen_digit = 0;
    int seen_point = 0;
    int point_position = 0;

    for (;; p++) {
        if (*p == '.' && !seen_point) {
            seen_point = 1;
            continue;
        }
        if (!isdigit((unsigned char)*p)) {
            break;
        }
        seen_digit = 1;
        if (seen_point) {
            point_position--;
        }
        if (digits < DIGIT_LIMIT) {
            if (mantissa != 0 || *p != '0') {
                mantissa = mantissa * 10u + (unsigned)(*p - '0');
                digits++;
            }
        } else {
            /* Past nineteen digits the integer would overflow. What is
               dropped still moves the exponent, and the digits themselves
               are below where this format can see them. */
            dropped++;
        }
    }

    if (!seen_digit) {
        return 0.0L;
    }

    int exponent = point_position + dropped;

    if (*p == 'e' || *p == 'E') {
        const char *after = p + 1;
        int exponent_negative = 0;
        if (*after == '+' || *after == '-') {
            exponent_negative = (*after == '-');
            after++;
        }
        if (isdigit((unsigned char)*after)) {
            long value = 0;
            while (isdigit((unsigned char)*after)) {
                if (value < EXPONENT_LIMIT) {
                    value = value * 10 + (*after - '0');
                }
                after++;
            }
            exponent += exponent_negative ? -(int)value : (int)value;
            p = after;
        }
    }

    if (end) {
        *end = (char *)p;
    }

    long double value = (long double)mantissa;
    if (mantissa == 0) {
        return negative ? -0.0L : 0.0L;
    }
    value = scale_by_power_of_ten(value, exponent);
    return negative ? -value : value;
}
