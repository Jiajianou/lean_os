/* user_space/bin/faulttest.c - M99's fixture for a fault a program can catch.
 *
 * M76 built signals a program can catch and excluded the synchronous
 * ones: SIGSEGV was uncatchable, and every ring-3 fault - a divide
 * error, an invalid opcode, an alignment check - was reported as
 * SIGSEGV and killed the process. M52's own note said why: "they share
 * one exit code because this project has no per-signal handling to tell
 * them apart with."
 *
 * It has since M76. M99 is where the fault path uses it, because
 * CPython's `faulthandler.enable()` installs handlers for SIGSEGV,
 * SIGFPE, SIGILL, SIGBUS and SIGABRT - and CPython's own test runner
 * calls it before running a single test, so the whole regression suite
 * failed at `sigaction(SIGSEGV)` returning -1.
 *
 * What this proves, in the order it proves it:
 *
 *   1. a null dereference runs a SIGSEGV handler and the process lives
 *   2. an integer divide by zero runs a SIGFPE handler - a DIFFERENT
 *      signal, which is the half M52 said this project could not do
 *   3. an opcode this CPU does not have runs a SIGILL handler
 *   4. all three again, which is the check that the mask came back:
 *      signal_deliver blocks a signal while its handler runs and
 *      siglongjmp is what puts it back, so a program that can survive
 *      one fault and not two has a mask leak rather than a fault bug
 *   5. a child whose SIGSEGV handler itself faults DIES, once. That is
 *      the loop being cut, and it is the property that makes catching a
 *      fault safe rather than a way to hang the machine
 *
 * Exit codes, so a failure names itself:
 *   0  everything worked
 *   2  signal() refused a handler for a fault it should now accept
 *   3  the SIGSEGV handler never ran
 *   4  the SIGFPE handler never ran
 *   5  the SIGILL handler never ran
 *   6  the second round of faults did not arrive - the mask leaked
 *   7  the child whose handler faults did not die
 *   8  the child whose handler faults died of the wrong thing
 */
#include <setjmp.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "syscall_wrappers.h"

static sigjmp_buf recover;
static volatile int segv_count;
static volatile int fpe_count;
static volatile int ill_count;

/* Each handler leaves by siglongjmp rather than by returning, and that
 * is not a style choice: returning from a fault handler re-executes the
 * instruction that faulted, which faults again. Unwinding to a known
 * point is what every real program that catches a fault does. */
static void on_segv(int sig) {
    (void)sig;
    segv_count++;
    siglongjmp(recover, 1);
}

static void on_fpe(int sig) {
    (void)sig;
    fpe_count++;
    siglongjmp(recover, 1);
}

static void on_ill(int sig) {
    (void)sig;
    ill_count++;
    siglongjmp(recover, 1);
}

/* volatile, so the compiler cannot decide these are undefined behaviour
 * and delete them. Each one is a fault the hardware raises. */
static volatile int *null_pointer;
static volatile int zero;

static void touch_null(void) {
    *null_pointer = 1;
}

static void divide_by_zero(void) {
    volatile int n = 1;
    n = n / zero;
    (void)n;
}

static void bad_opcode(void) {
    __asm__ volatile("ud2");
}

/* Runs `fault` with `handler` installed, and returns 1 if the handler
 * ran and control came back here. */
static int survives(int signo, void (*fault)(void)) {
    if (sigsetjmp(recover, 1) == 0) {
        fault();
        return 0; /* the fault did not happen at all */
    }
    (void)signo;
    return 1;
}


/* ---- M99: the SA_SIGINFO half ---------------------------------------- */

static sigjmp_buf info_recover;
static volatile void *seen_addr;
static volatile int seen_signo;
static volatile int seen_code;
static volatile int chld_pid;
static volatile int chld_status;
static volatile int chld_seen;

static void on_segv_info(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    seen_signo = sig;
    seen_addr = si->si_addr;
    seen_code = si->si_code;
    siglongjmp(info_recover, 1);
}

static void on_chld_info(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    (void)uc;
    chld_pid = si->si_pid;
    chld_status = si->si_status;
    chld_seen++;
}

/* A page nothing has mapped, well inside the address space and well
 * outside anything this program owns - so the faulting address is a
 * number chosen here and comparable afterwards, rather than 0, which
 * would pass even if si_addr were never written. */
static volatile int *const KNOWN_BAD = (volatile int *)0x00000000DEAD0000ULL;

static int siginfo_checks(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv_info;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, 0) != 0) {
        printf("faulttest: sigaction(SIGSEGV, SA_SIGINFO) refused\n");
        return 0;
    }

    if (sigsetjmp(info_recover, 1) == 0) {
        *KNOWN_BAD = 1;
        printf("faulttest: writing an unmapped page did not fault\n");
        return 0;
    }
    if (seen_signo != SIGSEGV) {
        printf("faulttest: the siginfo handler saw signal %d\n", seen_signo);
        return 0;
    }
    if (seen_addr != (void *)KNOWN_BAD) {
        printf("faulttest: si_addr was %p, the faulting address was %p\n",
               (void *)seen_addr, (void *)KNOWN_BAD);
        return 0;
    }

    /* And a query must not quietly downgrade it. sigaction(sig, NULL,
     * &old) is a read-modify-write on this system - it installs SIG_DFL
     * to learn the old handler and puts it back - so a version that put
     * it back with flags of zero would turn this three-argument handler
     * into a one-argument one by asking a question about it. */
    struct sigaction got;
    memset(&got, 0, sizeof(got));
    if (sigaction(SIGSEGV, 0, &got) != 0 || !(got.sa_flags & SA_SIGINFO)) {
        printf("faulttest: querying the handler lost SA_SIGINFO (flags 0x%x)\n",
               got.sa_flags);
        return 0;
    }
    seen_addr = 0;
    if (sigsetjmp(info_recover, 1) == 0) {
        *KNOWN_BAD = 1;
        printf("faulttest: the second fault did not arrive\n");
        return 0;
    }
    if (seen_addr != (void *)KNOWN_BAD) {
        printf("faulttest: after a query, si_addr was %p\n", (void *)seen_addr);
        return 0;
    }

    /* SIGCHLD, where the interesting fields are si_pid and si_status -
     * the two toybox's `timeout` reads and the two a siginfo_t full of
     * zeroes would answer wrongly without failing. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_chld_info;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGCHLD, &sa, 0) != 0) {
        printf("faulttest: sigaction(SIGCHLD, SA_SIGINFO) refused\n");
        return 0;
    }
    long pid = sys_fork();
    if (pid == 0) {
        _exit(42); /* a number nothing else here uses */
    }
    if (pid < 0) {
        printf("faulttest: could not fork for the SIGCHLD check\n");
        return 0;
    }
    long deadline = sys_uptime_ms() + 5000;
    while (chld_seen == 0 && sys_uptime_ms() < deadline) {
        sys_yield();
    }
    int status = 0;
    sys_waitpid(pid, &status, 0);
    if (chld_seen == 0) {
        printf("faulttest: no SIGCHLD arrived\n");
        return 0;
    }
    if (chld_pid != (int)pid || chld_status != 42) {
        printf("faulttest: SIGCHLD said pid %d status %d, the child was "
               "pid %ld exiting 42\n", chld_pid, chld_status, pid);
        return 0;
    }
    return 1;
}

int main(void) {
    null_pointer = (volatile int *)0;
    zero = 0;

    if (signal(SIGSEGV, on_segv) == SIG_ERR ||
        signal(SIGFPE, on_fpe) == SIG_ERR ||
        signal(SIGILL, on_ill) == SIG_ERR) {
        return 2;
    }

    if (!survives(SIGSEGV, touch_null) || segv_count != 1) {
        return 3;
    }
    if (!survives(SIGFPE, divide_by_zero) || fpe_count != 1) {
        return 4;
    }
    if (!survives(SIGILL, bad_opcode) || ill_count != 1) {
        return 5;
    }

    /* Round two. The mask, not the fault: signal_deliver blocked each of
     * these while its handler ran, and siglongjmp is what unblocked it
     * on the way out. Without that this round hangs or dies. */
    if (!survives(SIGSEGV, touch_null) || segv_count != 2) {
        return 6;
    }
    if (!survives(SIGFPE, divide_by_zero) || fpe_count != 2) {
        return 6;
    }
    if (!survives(SIGILL, bad_opcode) || ill_count != 2) {
        return 6;
    }

    printf("faulttest: three faults caught twice each - segv %d, fpe %d, ill %d\n",
           segv_count, fpe_count, ill_count);

    /* ---- and the loop, cut ------------------------------------------
     *
     * A child that installs a SIGSEGV handler which itself faults. The
     * signal is blocked while its own handler runs, so the second fault
     * finds nothing deliverable and the child is terminated with it -
     * once. A machine where that looped would be a machine any program
     * could hang by catching a fault badly. */
    long pid = sys_fork();
    if (pid == 0) {
        signal(SIGSEGV, (void (*)(int))touch_null); /* a handler that faults */
        *null_pointer = 1;
        _exit(0); /* not reached: the second fault is fatal */
    }
    if (pid < 0) {
        return 7;
    }
    int status = 0;
    if (sys_waitpid(pid, &status, 0) < 0) {
        return 7;
    }
    /* Through the macros rather than against a number. SYS_waitpid's
     * status is the POSIX encoding - a signal number in the low seven
     * bits for a death by signal, the exit code in the second byte for
     * an ordinary exit - and 139 is the SHELL's rendering of the same
     * fact, which is what SYS_wait returns and this is not. Written the
     * wrong way first, and the failure said "exited 11, not 139", which
     * is two correct numbers for two different questions. */
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        printf("faulttest: the looping child's status was 0x%x - "
               "signalled=%d signal=%d, wanted SIGSEGV (%d)\n",
               status, WIFSIGNALED(status) ? 1 : 0, WTERMSIG(status), SIGSEGV);
        return 8;
    }

    printf("faulttest: and a handler that faults kills its process once\n");

    /* ---- SA_SIGINFO, and the lie it used to be ----------------------
     *
     * `<signal.h>` said, in as many words, that "a handler installed
     * with SA_SIGINFO is called through sa_handler with the signal
     * number ... the pointer arguments are never passed". That is an
     * accurate description of calling a three-argument function with one
     * argument: the handler reads %rsi as a pointer and gets whatever
     * the last caller left there.
     *
     * toybox's `timeout` is what found it - its SIGCHLD handler's first
     * statement is `si->si_status`, and it faulted at address 5 (the
     * offset of that field) every single time it ran, which on this
     * machine was once per module of CPython's regression suite.
     *
     * Two claims here and they are different: that si_addr is the
     * address that actually faulted, and that si_pid/si_status describe
     * the child that actually ended. A siginfo_t full of zeroes would
     * pass a test that only checked the handler was reached with three
     * arguments. */
    if (!siginfo_checks()) {
        return 9;
    }
    printf("faulttest: and a three-argument handler gets a siginfo_t "
           "with the faulting address and the child's status in it\n");
    return 0;
}
