#ifndef LEANOS_TESTS_CLANG_ABI_H
#define LEANOS_TESTS_CLANG_ABI_H

#include <stdint.h>

struct two_longs {
    long a;
    long b;
};

struct two_floats {
    float x;
    float y;
};

struct int_and_double {
    long tag;
    double value;
};

struct big {
    long v[6];
};

struct packed_flags {
    unsigned int a : 3;
    unsigned int b : 5;
    _Bool set;
    char tail;
};

typedef int (*callback_function)(int, int);

struct two_longs        peer_two_longs(long a, long b);
struct two_floats       peer_two_floats(float x, float y);
struct int_and_double   peer_int_and_double(long tag, double value);
struct big              peer_big(long seed);
struct packed_flags     peer_packed_flags(unsigned a, unsigned b, int set);
long                    peer_sum_mixed_varargs(int count, ...);
long double             peer_long_double(long double v);
int                     peer_call_back(callback_function function, int a, int b);
long                    peer_take_two_longs(struct two_longs s);
double                  peer_take_int_and_double(struct int_and_double s);
long                    peer_take_big(struct big s);

int                     main_doubler(int a, int b);

#endif
