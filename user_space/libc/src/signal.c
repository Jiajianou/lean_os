#include <errno.h>
#include <signal.h>
#include <setjmp.h>

#include "syscall_wrappers.h"

extern void __lean_sigreturn(void);
const unsigned int __lean_sigreturn_number = SYS_sigreturn;

static int lean_sa_flags[SIG_MAX + 1];

sighandler_t signal(int sig, sighandler_t handler) {
    long previous = sys_sigaction(sig, (void *)handler, __lean_sigreturn, 0);
    if (previous < 0) {
        return SIG_ERR;
    }
    if (sig >= 0 && sig <= SIG_MAX) {
        lean_sa_flags[sig] = 0;
    }
    return (sighandler_t)previous;
}

/* Only a negative result is a refusal. raise() hands back whatever the
   syscall's RAX is when the signal returns, and basetest edits it in a
   ucontext to prove the edit takes effect. */
static int kill_result(long r) {
    if (r >= 0) {
        return (int)r;
    }
    errno = r == -OS_ERROR_SEARCH ? ESRCH : r == -OS_ERROR_PERMISSION ? EPERM : EINVAL;
    return -1;
}

int kill(int pid, int sig) {
    return kill_result(sys_kill(pid, sig));
}

int raise(int sig) {
    return kill_result(sys_kill(sys_getpid(), sig));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    unsigned int mask = set ? *set : 0u;
    if (!set) {
        how = SIG_BLOCK;
    }
    return (int)sys_sigprocmask(how, mask, old);
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    /* The one real difference from sigprocmask: this returns the error number
       and leaves errno alone. */
    if (sigprocmask(how, set, oldset) != 0) {
        return errno ? errno : EINVAL;
    }
    return 0;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig < 0 || sig > SIG_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!act) {
        long previous = sys_sigaction(sig, (void *)SIG_DFL, __lean_sigreturn, 0);
        if (previous < 0) {
            return -1;
        }
        sys_sigaction(sig, (void *)previous, __lean_sigreturn,
                      (unsigned int)lean_sa_flags[sig]);
        if (old) {
            old->sa_handler = (sighandler_t)previous;
            old->sa_mask = 0;
            old->sa_flags = lean_sa_flags[sig];
        }
        return 0;
    }
    long previous = sys_sigaction(sig, (void *)act->sa_handler, __lean_sigreturn,
                              (unsigned int)act->sa_flags);
    if (previous < 0) {
        return -1;
    }
    int previous_flags = lean_sa_flags[sig];
    lean_sa_flags[sig] = act->sa_flags;
    if (old) {
        old->sa_handler = (sighandler_t)previous;
        old->sa_mask = 0;
        old->sa_flags = previous_flags;
    }
    return 0;
}

int sigaltstack(const stack_t *new_stack, stack_t *old_stack) {
    os_stack_t want;
    os_stack_t had;
    if (new_stack) {
        want.base = (uint64_t)(uintptr_t)new_stack->ss_sp;
        want.size = (uint64_t)new_stack->ss_size;
        want.flags = (uint32_t)new_stack->ss_flags;
        want.reserved = 0;
    }
    if (sys_sigaltstack(new_stack ? &want : (os_stack_t *)0,
                        old_stack ? &had : (os_stack_t *)0) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (old_stack) {
        old_stack->ss_sp = (void *)(uintptr_t)had.base;
        old_stack->ss_size = (size_t)had.size;
        old_stack->ss_flags = (int)had.flags;
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
