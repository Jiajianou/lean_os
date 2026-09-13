#include <errno.h>
#include <signal.h>
#include <setjmp.h>

#include "syscall_wrappers.h"

extern void __lean_sigreturn(void);
const unsigned int __lean_sigreturn_number = SYS_sigreturn;

static int lean_sa_flags[SIG_MAX + 1];

sighandler_t signal(int sig, sighandler_t handler) {
    long prev = sys_sigaction(sig, (void *)handler, __lean_sigreturn, 0);
    if (prev < 0) {
        return SIG_ERR;
    }
    if (sig >= 0 && sig <= SIG_MAX) {
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

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    unsigned int mask = set ? *set : 0u;
    if (!set) {
        how = SIG_BLOCK;
    }
    return (int)sys_sigprocmask(how, mask, old);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig < 0 || sig > SIG_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!act) {
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

void __sigjmp_save(__sigjmp_state *env, int savemask) {
    if (!env) {
        return;
    }
    env->savemask = savemask;
    env->mask = 0;
    if (savemask) {
        sigprocmask(SIG_BLOCK, 0, &env->mask);
    }
}

void siglongjmp(sigjmp_buf env, int value) {
    if (env->savemask) {
        sigprocmask(SIG_SETMASK, &env->mask, 0);
    }
    longjmp(env->jb, value);
}
