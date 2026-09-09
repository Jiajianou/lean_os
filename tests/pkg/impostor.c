/* tests/pkg/impostor.c - M111
 *
 * A package that tries to be the compositor.
 *
 * It is built by tools/build-packages.sh with the x86_64-lean_os
 * compiler and installed as `bin/compositor` and `bin/shutdown` - two
 * names that are IN system_api/include/caps.h's grant table, with
 * CAP_ALL and CAP_POWER attached to them. Before M111, launching this
 * out of any directory on the machine would have been launching it with
 * those capabilities, because caps_for_program() matched on the basename
 * and every basename on the disk had come out of this repository.
 *
 * So this program exits with its own capability mask as its status, and
 * user_space/bin/pkgtest.c requires that number to be zero. It is the
 * shortest possible statement of the rule: what a program may do is
 * decided by where it was installed from and what its package asked for,
 * never by what it is called.
 *
 * It is a test fixture and not a package anybody would want, which is
 * why it lives in tests/ and is written here rather than ported. The
 * repository ships it on purpose: a boundary with no adversary in the
 * image is a boundary nothing checks.
 */
#include <stdio.h>
#include <unistd.h>

#include "syscall_wrappers.h"

int main(void) {
    unsigned mask = (unsigned)sys_getcaps();
    /* Printed as well as returned, because the exit status is what
     * pkgtest asserts on and the line is what a person reads when it
     * fails. */
    printf("impostor: I was launched as this program, holding 0x%x\n", mask);
    /* The status is the assertion, so it has to be nonzero for EVERY
     * nonzero mask. A capability mask is twelve bits (CAP_ALL is 0xFFF)
     * and an exit status is conventionally eight, so the top four bits
     * are folded down onto the bottom ones rather than truncated: a mask
     * of exactly 0x100 (fs-write alone, which is what a package that
     * asked for nothing must NOT have) would otherwise exit 0 and turn
     * this test green by accident. */
    unsigned folded = (mask & 0xFFu) | ((mask >> 8) & 0xFFu);
    return (int)folded;
}
