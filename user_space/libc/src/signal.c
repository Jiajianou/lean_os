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
