/* tests/fakes/fake_panic_abort.c - Q4
 *
 * panic() for the fuzzers, where it must abort rather than longjmp.
 *
 * The unit-test fake (fake_panic.c) catches a panic so a test can assert
 * one happened. A fuzzer wants the opposite: a panic is a finding, and
 * abort() is what makes libFuzzer stop, save the input and report it.
 *
 * Which is right depends on who is asking, and getting it wrong in either
 * direction is bad - a fuzzer that swallowed panics would report nothing,
 * and a test suite that aborted on one could not test them. Two files,
 * one linked into each. */
#include <stdio.h>
#include <stdlib.h>

void panic(const char *msg);

void panic(const char *msg) {
    fprintf(stderr, "*** KERNEL PANIC reached by a fuzz input: %s\n",
            msg ? msg : "(null)");
    abort();
}
