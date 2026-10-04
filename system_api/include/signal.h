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
#define SA_ONSTACK 0x08000000
#define SA_RESETHAND 0x80000000

#define OS_SS_ONSTACK 1
#define OS_SS_DISABLE 2

/* The minimum an alternate signal stack may be, and the size a program that
   has no opinion should ask for. A frame here is a sig_frame_t plus a
   siginfo_t plus an os_ucontext_t plus the 128-byte red zone the ABI
   reserves, so the floor is what that costs with room for the handler itself
   on top. M143 added the ucontext, which is 304 bytes of it. */
#define OS_MINSIGSTKSZ 4096
#define OS_SIGSTKSZ    16384

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} os_stack_t;

typedef struct {
    int si_signo;
    int si_code;
    int si_errno;
    int si_pid;
    unsigned int si_uid;
    void *si_addr;
    int si_status;
    long si_band;
    union {
        int sival_int;
        void *sival_pointer;
    } si_value;
} siginfo_t;

/* Who sent the signal, for the codes that are about an origin rather than a
   fault. POSIX names these five and says nothing about their values; the
   convention every system settled on is that a POSITIVE si_code means the
   kernel raised it from hardware and a non-positive one means somebody sent
   it, which is what lets a fault handler tell a real trap from a signal that
   merely arrived. The numbering below is that convention.

   This kernel produces SI_USER and the fault codes further down and nothing
   else, because it has no sigqueue, no POSIX timers delivering signals, no
   asynchronous I/O and no message queues. The names are here because a
   program that asks "was this a trap or was it sent" has to be able to write
   the question, and the answer it gets here is a truthful no. M155 added
   them: V8's trap handler is the program, and it decides whether a SIGSEGV
   came from a JavaScript heap access by exactly that test. */
#define SI_USER    0
#define SI_QUEUE   (-1)
#define SI_TIMER   (-2)
#define SI_MESGQ   (-3)
#define SI_ASYNCIO (-4)
#define SI_SIGIO   (-5)
#define SI_TKILL   (-6)

/* What kind of fault. The kernel knows - it has the trap vector, the page
   fault's error code and the two floating point status words - and before
   M144 it reported SI_KERNEL for all of them, which is "something went
   wrong" where the hardware had already said which thing. */
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define BUS_OBJERR 3

#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8

#define TRAP_BRKPT 1
#define TRAP_TRACE 2

#define ILL_ILLOPC 1
#define ILL_ILLOPN 2
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8
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
    (((sig) == SIGCHLD || (sig) == SIGWINCH || (sig) == SIGURG) ? SIG_DFL_IGNORE \
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
    /* Where the os_ucontext_t for this delivery sits, so sigreturn can take
       the registers back from it. That is what makes a handler's edit to
       uc_mcontext take effect instead of being quietly discarded, which is
       the difference between a ucontext and a report that looks like one. */
    uint64_t ucontext_address;
} sig_frame_t;

/* The register order is glibc's - see user_space/libc/include/sys/ucontext.h
   for why a differently ordered array would be worse than none. */
#define OS_REG_R8       0
#define OS_REG_R9       1
#define OS_REG_R10      2
#define OS_REG_R11      3
#define OS_REG_R12      4
#define OS_REG_R13      5
#define OS_REG_R14      6
#define OS_REG_R15      7
#define OS_REG_RDI      8
#define OS_REG_RSI      9
#define OS_REG_RBP     10
#define OS_REG_RBX     11
#define OS_REG_RDX     12
#define OS_REG_RAX     13
#define OS_REG_RCX     14
#define OS_REG_RSP     15
#define OS_REG_RIP     16
#define OS_REG_EFL     17
#define OS_REG_CSGSFS  18
#define OS_REG_ERR     19
#define OS_REG_TRAPNO  20
#define OS_REG_OLDMASK 21
#define OS_REG_CR2     22
#define OS_NGREG       23

typedef struct {
    int64_t gregs[OS_NGREG];
    uint64_t fpregs;
    uint64_t reserved[8];
} os_mcontext_t;

/* NOT os_stack_t. That one is this OS's own sigaltstack argument, and it
   orders its fields base/size/flags; the stack_t inside a ucontext is the
   one <signal.h> declares, and a handler reads it through that declaration.
   Two structures for the same three numbers, because two callers disagree
   about their order - basetest compares the offsets from both sides. */
typedef struct {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t reserved;
    uint64_t ss_size;
} os_signal_stack_t;

typedef struct {
    uint64_t uc_flags;
    uint64_t uc_link;
    os_signal_stack_t uc_stack;
    os_mcontext_t uc_mcontext;
    uint32_t uc_sigmask;
} os_ucontext_t;

#ifdef __cplusplus
}
#endif
