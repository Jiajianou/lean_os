#pragma once

#include_next <signal.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)SIG_DFL_ADDR)
#define SIG_IGN ((sighandler_t)SIG_IGN_ADDR)
#define SIG_ERR ((sighandler_t)-1)

sighandler_t signal(int sig, sighandler_t handler);

int kill(int pid, int sig);

int raise(int sig);

typedef unsigned int sigset_t;

typedef int sig_atomic_t;

int sigprocmask(int how, const sigset_t *set, sigset_t *old);

/* POSIX declares this in <signal.h> rather than <pthread.h>, and on this
   system it is sigprocmask: a signal mask belongs to a task, a thread IS
   a task, so sigprocmask already acts on the calling thread alone. The
   two differ only in how they report an error. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

struct sigaction {
    union {
        sighandler_t sa_handler;
        void       (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t     sa_mask;
    int          sa_flags;
};

#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_ONSTACK   0x08000000
#define SA_RESETHAND 0x80000000
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002

typedef struct {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

#define SS_ONSTACK OS_SS_ONSTACK
#define SS_DISABLE OS_SS_DISABLE

#define MINSIGSTKSZ OS_MINSIGSTKSZ
#define SIGSTKSZ    OS_SIGSTKSZ

int sigaltstack(const stack_t *new_stack, stack_t *old_stack);

int sigaction(int sig, const struct sigaction *act, struct sigaction *old);

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

#define NSIG (SIG_MAX + 1)

#ifdef __cplusplus
}
#endif
