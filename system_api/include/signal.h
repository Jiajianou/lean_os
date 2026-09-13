#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17

#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

#define SIGWINCH 28

#define SIGTRAP  5
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGIO    29
#define SIGSYS   31
#define SIGBUS   7

#define SIG_MAX  31

#define SIG_IS_CATCHABLE(sig) \
    ((sig) > 0 && (sig) <= SIG_MAX && (sig) != SIGKILL && (sig) != SIGSTOP)

#define SA_SIGINFO 0x00000004

typedef struct {
    int si_signo;
    int si_code;
    int si_errno;
    int si_pid;
    unsigned int si_uid;
    void *si_address;
    int si_status;
    long si_band;
    union {
        int sival_int;
        void *sival_pointer;
    } si_value;
} siginfo_t;

#define SI_USER    0
#define SI_KERNEL  0x80
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

#define SIG_DFL_TERMINATE 0
#define SIG_DFL_IGNORE    1
#define SIG_DFL_STOP      2
#define SIG_DFL_CONTINUE  3

#define SIG_DEFAULT_ACTION(sig)                                              \
    (((sig) == SIGCHLD || (sig) == SIGWINCH) ? SIG_DFL_IGNORE                \
     : ((sig) == SIGCONT ? SIG_DFL_CONTINUE                                  \
        : (((sig) == SIGSTOP || (sig) == SIGTSTP || (sig) == SIGTTIN ||      \
            (sig) == SIGTTOU)                                                \
               ? SIG_DFL_STOP                                                \
               : SIG_DFL_TERMINATE)))

#define SIG_DFL_ADDR 0UL
#define SIG_IGN_ADDR 1UL

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, rsp;
    uint32_t saved_blocked;
    uint32_t signo;
} sig_frame_t;

#ifdef __cplusplus
}
#endif
