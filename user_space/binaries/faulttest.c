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

static int survives(int signo, void (*fault)(void)) {
    if (sigsetjmp(recover, 1) == 0) {
        fault();
        return 0;
    }
    (void)signo;
    return 1;
}

static sigjmp_buf info_recover;
static volatile void *seen_address;
static volatile int seen_signo;
static volatile int seen_code;
static volatile int chld_pid;
static volatile int chld_status;
static volatile int chld_seen;

static void on_segv_info(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    seen_signo = sig;
    seen_address = si->si_address;
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
    if (seen_address != (void *)KNOWN_BAD) {
        printf("faulttest: si_addr was %p, the faulting address was %p\n",
               (void *)seen_address, (void *)KNOWN_BAD);
        return 0;
    }

    struct sigaction got;
    memset(&got, 0, sizeof(got));
    if (sigaction(SIGSEGV, 0, &got) != 0 || !(got.sa_flags & SA_SIGINFO)) {
        printf("faulttest: querying the handler lost SA_SIGINFO (flags 0x%x)\n",
               got.sa_flags);
        return 0;
    }
    seen_address = 0;
    if (sigsetjmp(info_recover, 1) == 0) {
        *KNOWN_BAD = 1;
        printf("faulttest: the second fault did not arrive\n");
        return 0;
    }
    if (seen_address != (void *)KNOWN_BAD) {
        printf("faulttest: after a query, si_addr was %p\n", (void *)seen_address);
        return 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_chld_info;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGCHLD, &sa, 0) != 0) {
        printf("faulttest: sigaction(SIGCHLD, SA_SIGINFO) refused\n");
        return 0;
    }
    long pid = sys_fork();
    if (pid == 0) {
        _exit(42);
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

    long pid = sys_fork();
    if (pid == 0) {
        signal(SIGSEGV, (void (*)(int))touch_null);
        *null_pointer = 1;
        _exit(0);
    }
    if (pid < 0) {
        return 7;
    }
    int status = 0;
    if (sys_waitpid(pid, &status, 0) < 0) {
        return 7;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        printf("faulttest: the looping child's status was 0x%x - "
               "signalled=%d signal=%d, wanted SIGSEGV (%d)\n",
               status, WIFSIGNALED(status) ? 1 : 0, WTERMSIG(status), SIGSEGV);
        return 8;
    }

    printf("faulttest: and a handler that faults kills its process once\n");

    if (!siginfo_checks()) {
        return 9;
    }
    printf("faulttest: and a three-argument handler gets a siginfo_t "
           "with the faulting address and the child's status in it\n");
    return 0;
}
