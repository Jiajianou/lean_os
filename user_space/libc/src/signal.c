/* user_space/libc/src/signal.c - M76 */
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

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

/* M99: what this process last asked for, per signal.
 *
 * The kernel holds the authoritative SA_SIGINFO bit and has no call that
 * reads it back, and the query path below is a read-modify-write - it
 * installs SIG_DFL to learn the previous handler and then puts it back.
 * Without a shadow, that put-back would restore the address with flags
 * of zero and silently turn a three-argument handler into a
 * one-argument one, which is the exact bug M99 was fixing, reintroduced
 * by the act of asking a question.
 *
 * It is also what makes `old->sa_flags` a real answer rather than the 0
 * it has always been. A shadow rather than a new syscall because it
 * cannot drift: nothing but this function installs a handler, an exec
 * zeroes both sides, and a fork copies both. */
static int lean_sa_flags[SIG_MAX + 1];

sighandler_t signal(int sig, sighandler_t handler) {
    /* signal() is the one-argument form by definition, so no
     * SA_SIGINFO. sigaction() below is where a program asks for the
     * other one. */
    long prev = sys_sigaction(sig, (void *)handler, __lean_sigreturn, 0);
    if (prev < 0) {
        return SIG_ERR;
    }
    if (sig >= 0 && sig <= SIG_MAX) {
        /* M99: signal() is the one-argument form, so it CLEARS the
         * shadow as well as the kernel's bit. A program that installs a
         * three-argument handler with sigaction and then calls signal()
         * for the same number has changed its mind, and both sides have
         * to hear that. */
        lean_sa_flags[sig] = 0;
    }
    return (sighandler_t)prev;
}

int kill(int pid, int sig) {
    return (int)sys_kill(pid, sig);
}

int raise(int sig) {
    return (int)sys_kill(sys_getpid(), sig);
}

/* M89: the POSIX signature. `set` may be NULL, which asks to read the
 * mask without changing it - and the kernel has no such command, so this
 * expresses it the only way it can: block nothing. SIG_BLOCK with an
 * empty set is a no-op on any mask, so the old value comes back
 * unchanged and nothing was modified on the way. */
int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    unsigned int mask = set ? *set : 0u;
    if (!set) {
        how = SIG_BLOCK;
    }
    return (int)sys_sigprocmask(how, mask, old);
}

/* ---- the POSIX spelling - see <signal.h> for what sa_flags does not do */


int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig < 0 || sig > SIG_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!act) {
        /* A pure query. There is no "read the handler without changing
         * it" syscall, so this puts back what it just read - observably
         * identical, because sys_sigaction returns the previous handler
         * and this installs the same one again, with the flags it had. */
        long prev = sys_sigaction(sig, (void *)SIG_DFL, __lean_sigreturn, 0);
        if (prev < 0) {
            return -1;
        }
        sys_sigaction(sig, (void *)prev, __lean_sigreturn,
                      (unsigned int)lean_sa_flags[sig]);
        if (old) {
            old->sa_handler = (sighandler_t)prev;
            old->sa_mask = 0;
            old->sa_flags = lean_sa_flags[sig];
        }
        return 0;
    }
    /* M99: sa_flags is passed through now, because the kernel reads one
     * bit of it. SA_SIGINFO says the address in sa_handler/sa_sigaction
     * is a three-argument function - the union makes the two fields one
     * address, and the flag is the only thing that says which shape it
     * has. Before this the flag was accepted and dropped, so a
     * three-argument handler was called with one argument and read %rsi
     * as a pointer; see system_api/include/signal.h for the program that
     * faulted on it every time. */
    long prev = sys_sigaction(sig, (void *)act->sa_handler, __lean_sigreturn,
                              (unsigned int)act->sa_flags);
    if (prev < 0) {
        return -1;
    }
    int prev_flags = lean_sa_flags[sig];
    lean_sa_flags[sig] = act->sa_flags;
    if (old) {
        old->sa_handler = (sighandler_t)prev;
        old->sa_mask = 0;
        old->sa_flags = prev_flags;
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

/* ---- M89: the mask half of sigsetjmp/siglongjmp ----------------------
 *
 * See <setjmp.h> for why this is split between a macro and these two
 * functions rather than being a pair of ordinary calls.
 */
void __sigjmp_save(__sigjmp_state *env, int savemask) {
    if (!env) {
        return;
    }
    env->savemask = savemask;
    env->mask = 0;
    if (savemask) {
        /* NULL `set` reads without changing - see sigprocmask above. */
        sigprocmask(SIG_BLOCK, 0, &env->mask);
    }
}

void siglongjmp(sigjmp_buf env, int value) {
    if (env->savemask) {
        sigprocmask(SIG_SETMASK, &env->mask, 0);
    }
    longjmp(env->jb, value);
}
