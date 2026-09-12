/* tests/clang/abi_peer.c - M121. Compiled by x86_64-lean_os-GCC.
 *
 * The other half of the cross-compiler ABI fixture. Every function here
 * is called from tests/clang/abi_main.c, which clang compiled, and one
 * function here calls back into it - see tests/clang/abi.h for why each
 * shape is on the list.
 *
 * Nothing in this file checks anything. It computes, and abi_main.c
 * decides: a fixture that asserted on both sides could agree with
 * itself while both sides were wrong.
 */
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

/* Mixed integer and floating-point varargs, which is the case that needs
 * the register save area and `al` to agree. */
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

/* x87 long double, 16 bytes wide with 10 bytes of it meaningful, passed
 * on the stack and returned in st0. */
long double peer_long_double(long double v) {
    return v * 3.0L + 1.0L;
}

int peer_call_back(callback_fn fn, int a, int b) {
    return fn(a, b) + 1;
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

/* And the direction that is easy to forget: GCC's code calling clang's.
 * abi_main.c exports main_doubler; this is what reaches it. */
int peer_reaches_main_doubler(int a, int b) {
    return main_doubler(a, b);
}
