/* user_space/bin/jobtest.c - M85's fixture
 *
 * A program that exists to be interrupted, suspended and resumed - which
 * are three things no program can do to itself, so all this does is be
 * available while the kernel-side self-test does them to it.
 *
 * It puts itself in its own process group first, because that is what a
 * job is: the terminal signals a *group*, and a program that stayed in
 * its parent's group would be testing whether the parent gets signalled.
 *
 * Exit codes:
 *   0  ran to completion without being disturbed
 *   2  could not become its own process group
 *   3  a SIGTSTP handler ran when the default (stop) was expected
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "syscall_wrappers.h"

int main(int argc, char **argv) {
    (void)argv;
    /* Its own group, and its own session when asked - a session leader is
     * what a shell is, and tcsetpgrp is only allowed from inside the
     * session that owns the terminal. */
    if (argc > 1) {
        if (sys_setsid() < 0) {
            return 2;
        }
    }
    if (sys_setpgid(0, 0) != 0) {
        return 2;
    }

    /* Long enough for the self-test to signal it several times and to
     * find it stopped in between. Every iteration yields, so a machine
     * with other work to do keeps doing it. */
    for (int i = 0; i < 20000; i++) {
        sys_yield();
    }
    printf("jobtest: finished undisturbed\n");
    return 0;
}
