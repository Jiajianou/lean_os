/* user_space/libc/include/signal.h - M76
 *
 * <signal.h> as a C program expects to find it, on top of
 * system_api/include/signal.h's numbers and frame layout. The two are
 * deliberately separate files: the system_api one is the kernel/user
 * contract and is included by the kernel, and a kernel has no business
 * seeing a `sighandler_t` typedef.
 *
 * Included by name, so this shadows nothing: the cross-toolchain ships no
 * libc headers of its own (see docs/toolchain.md).
 */
#pragma once

/* The numbers, SIG_IS_CATCHABLE and sig_frame_t, from
 * system_api/include/signal.h - which has the same name as this file and
 * sits later on the same include path (-Iuser_space/libc/include comes
 * before -Isystem_api/include in the Makefile). `#include_next` is
 * precisely the tool for that: continue the search from after wherever
 * this file was found. The alternative, a relative "../../../" path,
 * would hardcode this file's own location into it. */
#include_next <signal.h>

#include <sys/types.h> /* M89: pid_t and uid_t, for siginfo_t below */

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)SIG_DFL_ADDR)
#define SIG_IGN ((sighandler_t)SIG_IGN_ADDR)
#define SIG_ERR ((sighandler_t)-1)

/* Installs `handler` for `sig` and returns the previous one, or SIG_ERR.
 * The BSD/POSIX semantics rather than V7's: the handler stays installed
 * across a delivery, and the signal is blocked for the duration of its
 * own handler. A handler that has to be reinstalled on every delivery is
 * a race nobody can write around, and there is no reason to reproduce it
 * here. */
sighandler_t signal(int sig, sighandler_t handler);

/* Sends `sig` to `pid`. sig 0 asks only whether the process exists and
 * may be signalled. */
int kill(int pid, int sig);

/* Sends `sig` to this process. Returns 0, or -1. */
int raise(int sig);

/* ---- M89: sigprocmask takes pointers, because POSIX says so ----------
 *
 * This was `sigprocmask(int how, unsigned int mask, unsigned int *old)`
 * from M76 to M89, on the argument that the kernel's mask is a plain
 * word and a pointer to one would be ceremony. That was a reasonable
 * design for a system whose only callers were in this tree, and it is
 * the wrong one the moment a program written elsewhere calls it: toybox
 * passes `&sigset` and gets a diagnostic about making an integer from a
 * pointer, which is the compiler catching a real ABI difference rather
 * than a spelling one.
 *
 * So the POSIX signature wins. `sigset_t` is still a plain 32-bit word
 * (see below for why an opaque struct would be a wrapper around a
 * number), so the change is entirely in how it is passed. `set` may be
 * NULL to read the mask without changing it, which is the form a program
 * uses to save it - and which the old signature could not express at
 * all.
 */
typedef unsigned int sigset_t;

/* M97: <csignal> does `using ::sig_atomic_t;`. An int, which is what it
 * is everywhere: the type an object can have when it is written by a
 * signal handler and read by the interrupted code without tearing. On
 * x86-64 an aligned int load or store is a single instruction, which is
 * the whole of the guarantee. */
typedef int sig_atomic_t;

int sigprocmask(int how, const sigset_t *set, sigset_t *old);

/* ---- the POSIX spelling -----------------------------------------------
 *
 * `sigaction` over the same SYS_sigaction `signal` uses. It exists
 * because a program written elsewhere calls it - CPython's own
 * Python/pylifecycle.c does, which is where this project first needed
 * one - and because it is the call that lets a program *read back* what
 * a handler currently is.
 *
 * `sigset_t` is a plain 32-bit word, the same one SYS_sigprocmask takes.
 * POSIX makes it opaque so an implementation can carry more than 32
 * signals; this one has 31 (SIG_MAX) and will not grow, so an opaque
 * struct would be a wrapper around a number for the sake of a
 * possibility that is not coming.
 *
 * `sa_flags` is accepted and ignored, with one exception worth naming:
 * SA_RESTART asks that an interrupted syscall be restarted rather than
 * returning short, and this kernel always returns short (see M76's note
 * on interruptible blocking). A program that sets it and does not loop
 * is broken here; one that loops - which is what every program that
 * handles EINTR correctly already does - works either way.
 */
struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;   /* accepted; the handler's own signal is blocked regardless */
    int          sa_flags;
    void       (*sa_sigaction)(int, void *, void *); /* never called - SA_SIGINFO is not supported */
};

#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESETHAND 0x80000000
/* M89: accepted and ignored, and each for a reason worth one line.
 * SA_NOCLDSTOP asks not to be sent SIGCHLD when a child *stops* - this
 * kernel only sends SIGCHLD on exit (see sched_raise_signal), so the
 * flag describes a delivery that never happens. SA_NOCLDWAIT asks that
 * children be reaped automatically; SYS_wait's bookkeeping is what makes
 * a child reapable exactly once, and skipping it would leak task slots.
 */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002

/* M89: `siginfo_t`, declared so that a program which writes a
 * three-argument handler compiles.
 *
 * **Nothing ever fills one in.** SA_SIGINFO is not supported - see
 * `sa_sigaction` above, which says the same thing - because delivering
 * one would mean the kernel building a second, larger frame on the
 * process's own stack for information (a faulting address, a sending
 * uid) that this machine either does not have or has already reported
 * another way. A handler installed with SA_SIGINFO is called through
 * sa_handler with the signal number, which is the one argument that is
 * always right; the pointer arguments are never passed.
 *
 * The fields are the POSIX-required ones, so that the day this kernel
 * does fill one in it is not a second struct to reconcile. */
typedef struct {
    int si_signo;
    int si_code;
    int si_errno;
    pid_t si_pid;
    uid_t si_uid;
    void *si_addr;
    int si_status;
    long si_band;
    union {
        int sival_int;
        void *sival_ptr;
    } si_value;
} siginfo_t;

/* si_code values a program tests for. All of them are 0 here, because
 * nothing fills a siginfo_t in. */
#define SI_USER    0
#define SI_KERNEL  0x80
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

int sigaction(int sig, const struct sigaction *act, struct sigaction *old);

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

/* One past the highest signal number, the way every program that loops
 * over signals spells it. SIG_MAX is the highest; NSIG is the bound. */
#define NSIG (SIG_MAX + 1)


#ifdef __cplusplus
}
#endif
