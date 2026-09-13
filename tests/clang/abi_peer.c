#include "abi.h"

#include <stdarg.h>

struct two_longs peer_two_longs(long a, long b) {
    struct two_longs r;
    r.a = a * 3;
    r.b = b * 5;
    return r;
}

struct two_floats peer_two_floats(float x, float y) {
    struct two_floats r;
    r.x = x + 0.5f;
    r.y = y - 0.25f;
    return r;
}

struct int_and_double peer_int_and_double(long tag, double value) {
    struct int_and_double r;
    r.tag = tag + 100;
    r.value = value * 2.0;
    return r;
}

struct big peer_big(long seed) {
    struct big r;
    for (int i = 0; i < 6; i++) {
        r.v[i] = seed + i;
    }
    return r;
}

struct packed_flags peer_packed_flags(unsigned a, unsigned b, int set) {
    struct packed_flags r;
    r.a = a & 7u;
    r.b = b & 31u;
    r.set = set ? 1 : 0;
    r.tail = (char)(a + b);
    return r;
}

long peer_sum_mixed_varargs(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long total = 0;
    for (int i = 0; i < count; i++) {
        total += va_arg(ap, int);
        total += (long)va_arg(ap, double);
    }
    va_end(ap);
    return total;
}

long double peer_long_double(long double v) {
    return v * 3.0L + 1.0L;
}

int peer_call_back(callback_function function, int a, int b) {
    return function(a, b) + 1;
}

long peer_take_two_longs(struct two_longs s) {
    return s.a * 10 + s.b;
}

double peer_take_int_and_double(struct int_and_double s) {
    return (double)s.tag + s.value;
}

long peer_take_big(struct big s) {
    long total = 0;
    for (int i = 0; i < 6; i++) {
        total += s.v[i];
    }
    return total;
}

int peer_reaches_main_doubler(int a, int b) {
    return main_doubler(a, b);
}
