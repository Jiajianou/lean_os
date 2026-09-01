/* tests/fakes/fake_panic.c - Q2
 *
 * panic() as a longjmp rather than a halt. See tests/check.h's header for
 * why this is the piece that unlocks a whole category of test.
 *
 * When a test has not armed a catch, an unexpected panic is a hard exit
 * with the message - deliberately not a silent return, because a panic
 * the harness swallowed would let every assertion after it run against a
 * kernel that has already declared its own state impossible. */
#include "../check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *msg);

void panic(const char *msg) {
    snprintf(test_panic_msg, sizeof(test_panic_msg), "%s", msg ? msg : "(null)");
    if (test_panic_armed) {
        longjmp(test_panic_jmp, 1);
    }
    fprintf(stderr, "\n*** unexpected KERNEL PANIC in a host test: %s\n",
            test_panic_msg);
    exit(3);
}
