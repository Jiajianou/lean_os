/* user_space/libc/src/signal.c - M76 */
#include <signal.h>

#include "syscall_wrappers.h"

/* The restorer, and how its address gets to the kernel.
 *
 * `__lean_sigreturn` is in crt0.asm - it cannot be written in C, because
 * what it needs is the value RSP holds before any instruction has run.
 * `__lean_sigreturn_number` exists so that assembly does not have to
 * restate a syscall number the ABI header already owns; nasm has no way
 * to read a C header, and a hand-copied 73 in a .asm file is exactly the
 * kind of drift system_api/README.md exists to prevent. */
extern void __lean_sigreturn(void);
const unsigned int __lean_sigreturn_number = SYS_sigreturn;

sighandler_t signal(int sig, sighandler_t handler) {
    long prev = sys_sigaction(sig, (void *)handler, __lean_sigreturn);
    if (prev < 0) {
        return SIG_ERR;
    }
    return (sighandler_t)prev;
}

int kill(int pid, int sig) {
    return (int)sys_kill(pid, sig);
}

int raise(int sig) {
    return (int)sys_kill(sys_getpid(), sig);
}

int sigprocmask(int how, unsigned int mask, unsigned int *old) {
    return (int)sys_sigprocmask(how, mask, old);
}

/* ---- the POSIX spelling - see <signal.h> for what sa_flags does not do */

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (!act) {
        /* A pure query. There is no "read the handler without changing
         * it" syscall, so this puts back what it just read - observably
         * identical, because sys_sigaction returns the previous handler
         * and this installs the same one again. */
        long prev = sys_sigaction(sig, (void *)SIG_DFL, __lean_sigreturn);
        if (prev < 0) {
            return -1;
        }
        sys_sigaction(sig, (void *)prev, __lean_sigreturn);
        if (old) {
            old->sa_handler = (sighandler_t)prev;
            old->sa_mask = 0;
            old->sa_flags = 0;
            old->sa_sigaction = 0;
        }
        return 0;
    }
    long prev = sys_sigaction(sig, (void *)act->sa_handler, __lean_sigreturn);
    if (prev < 0) {
        return -1;
    }
    if (old) {
        old->sa_handler = (sighandler_t)prev;
        old->sa_mask = 0;
        old->sa_flags = 0;
        old->sa_sigaction = 0;
    }
    return 0;
}

int sigemptyset(sigset_t *set) {
    if (!set) {
        return -1;
    }
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (!set) {
        return -1;
    }
    /* Every signal, including the two that cannot be blocked - the mask
     * is a request, and SYS_sigprocmask is where SIGKILL and SIGSEGV are
     * cleared from it. Filling here and clearing there keeps the refusal
     * in exactly one place. */
    *set = 0xFFFFFFFFu;
    return 0;
}

int sigaddset(sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig > SIG_MAX) {
        return -1;
    }
    *set |= (1u << sig);
    return 0;
}

int sigdelset(sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig > SIG_MAX) {
        return -1;
    }
    *set &= ~(1u << sig);
    return 0;
}

int sigismember(const sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig > SIG_MAX) {
        return -1;
    }
    return (*set & (1u << sig)) ? 1 : 0;
}
