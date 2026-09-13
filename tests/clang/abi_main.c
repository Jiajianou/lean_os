#include "abi.h"

#include <stdio.h>

int peer_reaches_main_doubler(int a, int b);

int main_doubler(int a, int b) {
    return (a + b) * 2;
}

static int fails;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("mixedtest: FAIL %s\n", what);
        fails++;
    }
}

static int close_enough(double a, double b) {
    double d = a - b;
    return d > -0.0001 && d < 0.0001;
}

int main(void) {
    struct two_longs tl = peer_two_longs(7, 9);
    check(tl.a == 21 && tl.b == 45, "a struct of two longs, returned");

    struct two_floats tf = peer_two_floats(1.0f, 2.0f);
    check(close_enough(tf.x, 1.5) && close_enough(tf.y, 1.75),
          "a struct of two floats, returned in one SSE register");

    struct int_and_double iad = peer_int_and_double(5, 2.5);
    check(iad.tag == 105 && close_enough(iad.value, 5.0),
          "a mixed integer/SSE struct, returned");

    struct big big = peer_big(1000);
    int big_ok = 1;
    for (int i = 0; i < 6; i++) {
        if (big.v[i] != 1000 + i) {
            big_ok = 0;
        }
    }
    check(big_ok, "a struct returned through a hidden pointer");

    struct packed_flags pf = peer_packed_flags(5, 21, 1);
    check(pf.a == 5 && pf.b == 21 && pf.set == 1 && pf.tail == (char)26,
          "bitfields and a _Bool, laid out the same way");

    long mixed = peer_sum_mixed_varargs(2, 1, 2.0, 3, 4.0);
    check(mixed == 10, "mixed integer and floating-point varargs");

    long double ld = peer_long_double(2.5L);
    check(ld > 8.4999L && ld < 8.5001L, "an x87 long double, both ways");

    check(peer_call_back(main_doubler, 3, 4) == 15,
          "GCC's code calling a function clang compiled");

    check(peer_reaches_main_doubler(10, 11) == 42,
          "the same call through a second level");

    struct two_longs arg = {4, 6};
    check(peer_take_two_longs(arg) == 46,
          "a struct of two longs, passed by value");

    struct int_and_double iarg = {3, 1.5};
    check(close_enough(peer_take_int_and_double(iarg), 4.5),
          "a mixed struct, passed by value");

    struct big barg;
    for (int i = 0; i < 6; i++) {
        barg.v[i] = i + 1;
    }
    check(peer_take_big(barg) == 21, "a large struct, passed on the stack");

    if (fails) {
        printf("mixedtest: %d of the checks disagreed\n", fails);
        return 1;
    }
    printf("mixedtest: clang and gcc agree about this target's ABI - "
           "12 checks, both directions\n");
    return 0;
}
