/* user_space/bin/sigtest.c - M76's fixture
 *
 * What the milestone says would prove it: "A self-test program installs a
 * SIGINT handler; a driven Ctrl+C is asserted to run the handler and leave
 * the process alive - still listed in SYS_taskinfo afterwards - rather
 * than disappearing the way every process on this machine does today."
 *
 * So this program's whole job is to be that process. It installs
 * handlers, tells its parent it is ready by creating a file, and then
 * waits - staying alive is the assertion, and the kernel-side [m76]
 * self-test is what checks it.
 *
 * It also proves the two things a handler is actually for, which a bare
 * "did it run" would not:
 *
 *   - the interrupted context comes back. A counter is incremented in the
 *     handler and a local variable is checked afterwards; a sigreturn
 *     that restored the wrong registers shows up as a wrong number rather
 *     than as a crash, which is the failure mode worth catching.
 *   - SIGCHLD arrives without anyone polling for it. The child here exits
 *     immediately and this program never calls wait until afterwards.
 *
 * Exit codes, so a failure names itself:
 *   0  everything worked
 *   2  signal() refused a handler it should have accepted
 *   3  signal() accepted one it should have refused (SIGKILL)
 *   4  raise(SIGUSR1) did not run the handler
 *   5  the interrupted computation did not survive the handler
 *   6  SIGCHLD never arrived
 *   7  a blocked signal was delivered anyway
 *   8  the parent's SIGINT never arrived within the deadline
 */
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "syscall_wrappers.h"

static volatile int usr1_count;
static volatile int int_count;
static volatile int chld_count;
static volatile int usr2_count;

static void on_usr1(int sig) {
    (void)sig;
    usr1_count++;
}

static void on_int(int sig) {
    (void)sig;
    int_count++;
}

static void on_chld(int sig) {
    (void)sig;
    chld_count++;
}

static void on_usr2(int sig) {
    (void)sig;
    usr2_count++;
}

int main(void) {
    if (signal(SIGUSR1, on_usr1) == SIG_ERR ||
        signal(SIGUSR2, on_usr2) == SIG_ERR ||
        signal(SIGINT, on_int) == SIG_ERR ||
        signal(SIGCHLD, on_chld) == SIG_ERR) {
        return 2;
    }
    /* SIGKILL must be refused. A signal model where everything is
     * catchable is a machine with no way to stop a program. */
    if (signal(SIGKILL, on_usr1) != SIG_ERR) {
        return 3;
    }

    /* ---- the handler runs, and the interrupted work survives it -----
     *
     * `acc` is computed across the raise deliberately: the handler is
     * entered between two arithmetic steps, and a sigreturn that put back
     * the wrong registers would come out with a different total rather
     * than with a fault. */
    volatile long acc = 0;
    for (int i = 1; i <= 100; i++) {
        acc += i;
        if (i == 50) {
            raise(SIGUSR1);
        }
    }
    if (usr1_count != 1) {
        return 4;
    }
    if (acc != 5050) {
        return 5;
    }

    /* ---- a blocked signal waits ------------------------------------- */
    sigset_t usr2 = 1u << SIGUSR2;
    sigprocmask(SIG_BLOCK, &usr2, 0);
    raise(SIGUSR2);
    /* A syscall, so that delivery would have happened by now if the mask
     * were not holding it. */
    sys_uptime_ms();
    if (usr2_count != 0) {
        return 7;
    }
    sigprocmask(SIG_UNBLOCK, &usr2, 0);
    sys_uptime_ms();
    if (usr2_count != 1) {
        return 7;
    }

    /* ---- SIGCHLD, without polling for it ----------------------------- */
    long child = sys_spawn(PATH_BIN_DIR "hello", "");
    if (child >= 0) {
        long deadline = sys_uptime_ms() + 4000;
        while (chld_count == 0 && sys_uptime_ms() < deadline) {
            sys_yield();
        }
        sys_wait(child); /* reap it, now that the signal already said so */
    }
    if (chld_count == 0) {
        return 6;
    }

    /* ---- ready, and then alive ---------------------------------------
     *
     * The file is how the parent knows to send its SIGINT. A fixed sleep
     * would be a race in whichever direction the machine happened to be
     * slow that boot. */
    long fd = sys_open(PATH_TMP_DIR "m76ready", OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd >= 0) {
        sys_write((int)fd, "r", 1);
        sys_close((int)fd);
    }

    long deadline = sys_uptime_ms() + 15000;
    while (int_count == 0 && sys_uptime_ms() < deadline) {
        sys_yield();
    }
    if (int_count == 0) {
        return 8;
    }

    /* Still here. That is the whole claim: this process was sent a signal
     * that ends every other process on this machine, ran code in
     * response, and carried on. The parent checks SYS_taskinfo for it
     * before letting this exit. */
    fd = sys_open(PATH_TMP_DIR "m76alive", OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd >= 0) {
        char msg[64];
        int n = 0;
        const char *p = "handled-and-alive ";
        for (; *p; p++) {
            msg[n++] = *p;
        }
        msg[n++] = (char)('0' + (int_count % 10));
        sys_write((int)fd, msg, (size_t)n);
        sys_close((int)fd);
    }

    /* Waits to be told to go, so the parent can see this process listed
     * as living after the signal rather than racing its exit. */
    deadline = sys_uptime_ms() + 15000;
    while (usr1_count < 2 && sys_uptime_ms() < deadline) {
        sys_yield();
    }
    return 0;
}
