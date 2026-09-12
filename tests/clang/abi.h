/* tests/clang/abi.h - M121.
 *
 * The declarations shared by the two halves of the cross-compiler ABI
 * fixture: tests/clang/abi_main.c is compiled by clang and
 * tests/clang/abi_peer.c by GCC, and they are linked into one program.
 *
 * ---- why this fixture exists -----------------------------------------
 *
 * A second compiler for a target is not finished when it produces a
 * working program. It is finished when its objects can be linked with
 * the first one's, because that is what a real build does: this
 * project's libc.a, libgcc.a, crt1.o and the fifteen third-party
 * archives in the sysroot were all produced by GCC, and every program
 * clang compiles here links against them. If the two front ends
 * disagreed about how a struct is returned or where a varargs argument
 * sits, every one of those links would produce a program that runs and
 * is wrong - which is the worst shape a toolchain bug can take, and one
 * that no single-compiler test can see.
 *
 * So each declaration below is a place where the x86-64 System V
 * classification is doing real work, and the two halves are compiled by
 * different compilers on purpose. See tools/clang-test.sh for the two
 * command lines.
 */
#ifndef LEANOS_TESTS_CLANG_ABI_H
#define LEANOS_TESTS_CLANG_ABI_H

#include <stdint.h>

/* INTEGER, INTEGER - returned in rax:rdx, passed in rdi:rsi. */
struct two_longs {
    long a;
    long b;
};

/* SSE, SSE - returned in xmm0 (both halves packed into one register,
 * which is the classification rule people most often get wrong). */
struct two_floats {
    float x;
    float y;
};

/* INTEGER, SSE - rax and xmm0 on the way out; rdi and xmm0 on the way
 * in. A mixed eightbyte pair is the sharpest case in the whole ABI. */
struct int_and_double {
    long tag;
    double value;
};

/* MEMORY - larger than two eightbytes, so it is returned through a
 * hidden pointer in rdi and every other argument shifts along by one. */
struct big {
    long v[6];
};

/* A bitfield and a _Bool, which are laid out by rules the two compilers
 * implement independently. */
struct packed_flags {
    unsigned int a : 3;
    unsigned int b : 5;
    _Bool set;
    char tail;
};

typedef int (*callback_fn)(int, int);

/* Implemented in abi_peer.c (GCC), called from abi_main.c (clang). */
struct two_longs        peer_two_longs(long a, long b);
struct two_floats       peer_two_floats(float x, float y);
struct int_and_double   peer_int_and_double(long tag, double value);
struct big              peer_big(long seed);
struct packed_flags     peer_packed_flags(unsigned a, unsigned b, int set);
long                    peer_sum_mixed_varargs(int count, ...);
long double             peer_long_double(long double v);
int                     peer_call_back(callback_fn fn, int a, int b);
long                    peer_take_two_longs(struct two_longs s);
double                  peer_take_int_and_double(struct int_and_double s);
long                    peer_take_big(struct big s);

/* Implemented in abi_main.c (clang), called from abi_peer.c (GCC) - so
 * the agreement is tested in both directions rather than one. */
int                     main_doubler(int a, int b);

#endif /* LEANOS_TESTS_CLANG_ABI_H */
