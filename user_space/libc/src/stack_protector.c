#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

/* The stack protector's runtime. Nothing this project compiles has needed it
   - USER_CFLAGS says -fno-stack-protector - but Chromium compiles with
   -fstack-protector, and code compiled with it references these two names.
 *
 * The guard is random per process rather than a constant, which is the whole
 * point of the mechanism: a constant one is a constant an attacker can write
 * back. It is seeded in __lean_start before anything else runs, because a
 * function that sets up the guard cannot itself be protected by it.
 */

/* The value the compiler places below a protected function's return address
   and checks on the way out. */
uintptr_t __stack_chk_guard = 0x00000AFF0D15EA5EULL;

void __lean_stack_protector_init(void) {
    uintptr_t value = 0;
    if (getrandom(&value, sizeof(value), 0) == (ssize_t)sizeof(value)) {
        /* A null byte in the low position stops a string copy from running
           past the guard and rewriting it, which is what the terminator
           canary is for. */
        value &= ~(uintptr_t)0xFF;
        __stack_chk_guard = value;
    }
}

void __stack_chk_fail(void) {
    /* Async-signal-safe and allocation-free: whatever overwrote the guard
       has already corrupted the stack this would return through, so this
       says so on the descriptor it can reach and stops. */
    static const char message[] =
        "*** stack smashing detected: the guard below a return address was "
        "overwritten ***\n";
    (void)write(2, message, sizeof(message) - 1);
    _exit(134);
}

void __stack_chk_fail_local(void) {
    __stack_chk_fail();
}
