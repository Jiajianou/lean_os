/* system_api/include/signal.h
 *
 * M14 shipped this as "the basic set" - two numbers, no handlers, no
 * masking, and a comment saying so. What that bought was SYS_kill, which
 * is a way to *end* a process and nothing else. M76 is the other half:
 * a signal a program can catch, which is the difference between "a
 * running program can be told something happened" and "a running program
 * can be stopped".
 *
 * What is here, and what deliberately is not:
 *
 *  - handlers, installed with SYS_sigaction and invoked in ring 3 through
 *    a kernel-built frame on the process's own stack. See sig_frame_t
 *    below and kernel/arch/x86_64/syscall.c's delivery block.
 *  - a block/unblock mask (SYS_sigprocmask), because a handler that can
 *    be re-entered by its own signal is a handler nobody can write
 *    correctly. The mask is a plain 32-bit word, not POSIX's sigset_t
 *    with its four functions for setting bits in it.
 *  - no realtime signals, no queueing (a second SIGINT arriving before
 *    the first is handled is the same one bit), no SA_RESTART, no
 *    sigsuspend, no per-signal alternate stacks. Each of those is a real
 *    feature with a real cost and none of them is what "tell a program
 *    something happened" needs.
 *
 * The numbers match POSIX, as M14's did, so a program written elsewhere
 * means what it says here.
 */
#pragma once

#include <stdint.h>

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

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
/* Not catchable, not blockable, not ignorable - the one signal whose
 * whole purpose is that a process cannot argue with it. */
#define SIGKILL  9
#define SIGUSR1  10
/* M52: raised by the kernel at a ring-3 fault, so that a task killed for
 * faulting has an exit code that says so through the same "128 + signal"
 * convention every other death here already uses (139). Not catchable:
 * a handler for it would run on the address space that just faulted. */
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
/* M76: a child of yours ended. The one signal here whose default action
 * is to be ignored rather than to kill - which is why SYS_wait's own
 * comment has said "more complete wait semantics" was deferred since M14,
 * and why a parent has had to poll ever since. */
#define SIGCHLD  17

/* ---- M85: the job-control signals -------------------------------------
 *
 * The five that make a terminal able to interrupt, suspend and resume the
 * program in front of it. Their numbers are Linux's, like every other
 * number in this file, so a program written elsewhere means what it says.
 *
 * Until M85 every signal here either killed the process or was ignored,
 * which is why sched_raise_signal could get away with "death for
 * everything except SIGCHLD" and a comment saying a table of one row is a
 * table nobody reads. These add two more default actions - stop and
 * continue - and that comment is now a table worth writing.
 *
 * SIGSTOP joins SIGKILL as a signal a process cannot argue with, and for
 * the same kind of reason: a program able to catch or block it could
 * make itself unsuspendable, which is exactly the thing a terminal needs
 * to be able to do. SIGTSTP is the catchable one - it is what ^Z sends,
 * and an editor with unsaved work is entitled to hear about it first. */
#define SIGCONT  18 /* default: resume a stopped process. Cannot be ignored into uselessness - see sched_raise_signal */
#define SIGSTOP  19 /* default: stop. Uncatchable, unblockable, unignorable */
#define SIGTSTP  20 /* default: stop. What ^Z sends, and catchable so a program can save first */
#define SIGTTIN  21 /* default: stop. A background process tried to read the terminal */
#define SIGTTOU  22 /* default: stop. A background process tried to write it */

/* M89: the terminal changed size.
 *
 * Linux's number, like every other number in this file. Nothing on this
 * machine raises it yet - the console is a fixed 80x25 and the framebuffer
 * terminal is a program rather than a driver - so the only thing that can
 * send one today is `kill`. It is here because a program that draws to a
 * terminal installs a handler for it unconditionally, and a system that
 * cannot even name the signal makes that program fail to compile rather
 * than fail to resize. Its default action is to be ignored, which is the
 * one place a signal nobody sends still has to behave correctly: a
 * process that is sent one must not die. */
#define SIGWINCH 28

/* ---- M89: the rest of POSIX's numbers -------------------------------
 *
 * Nine signals nothing on this machine raises, given their Linux numbers
 * because a program that prints a signal name from a number must not
 * print the wrong one - and toybox's `kill -l` builds a table indexed by
 * every one of these.
 *
 * They are not stubs and they are not fictions: a number is a name, and
 * naming SIGXFSZ does not claim that anything enforces a file-size
 * limit. What each one would mean here, so the list is a fact rather
 * than a copy:
 *
 *   SIGTRAP  - a debugger's breakpoint. There is no debugger.
 *   SIGBUS   - an unaligned or unbacked access. M99: this is real now.
 *              An alignment check (#AC) from ring 3 raises SIGBUS, a
 *              divide error or a floating-point exception raises SIGFPE,
 *              and an opcode this CPU does not have raises SIGILL - see
 *              kernel/arch/x86_64/isr.c. M52's "every ring-3 fault is
 *              SIGSEGV" was true when nothing could catch one; the note
 *              it left - "they share one exit code because this project
 *              has no per-signal handling to tell them apart with" - is
 *              what stopped being true.
 *   SIGURG   - out-of-band TCP data. M66's TCP does not implement the
 *              urgent pointer.
 *   SIGXCPU  - a CPU-time limit. M88's getrlimit reports limits and
 *              nothing enforces one.
 *   SIGXFSZ  - a file-size limit. Same.
 *   SIGVTALRM/SIGPROF - interval timers. There is no setitimer here.
 *   SIGIO    - a descriptor became ready. That is what SYS_waitfds is
 *              for, and it is a wait rather than a signal.
 *   SIGSYS   - a bad system call. This kernel returns -1 instead, which
 *              is what every syscall here does for everything.
 *
 * A process CAN be sent any of them with kill(2), and the default action
 * for all nine is to terminate - which is the POSIX default and means a
 * `kill -TRAP` does what a person typing it expects. */
#define SIGTRAP  5
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGIO    29
#define SIGSYS   31
/* SIGBUS is 7 on Linux and 10 on the BSDs; 7 is the number a program
 * compiled for the same ABI as everything else in this file expects. */
#define SIGBUS   7

/* The highest signal number this kernel will carry. A task's pending and
 * blocked sets are single 32-bit words, so this is 31 and the arithmetic
 * that depends on it is in one place. */
#define SIG_MAX  31

/* 1 if `sig` is a signal a process may install a handler for, block, or
 * ignore. SIGKILL and SIGSTOP are the two that cannot be argued with,
 * both by definition. Written as a macro rather than a table so the
 * kernel and user space cannot hold two different opinions about it.
 *
 * ---- M99: SIGSEGV moved from that list to this one ------------------
 *
 * M76 excluded it with a real argument - "its handler would run on the
 * address space that just faulted" - and that argument is true and is
 * not sufficient. Every Unix lets a program catch SIGSEGV, and the
 * programs that do are not being clever: a garbage collector uses it for
 * write barriers, a JIT for lazy compilation, a runtime for
 * stack-overflow detection, and a crash reporter to say what happened
 * before it dies.
 *
 * CPython is the one that forced it here. `faulthandler.enable()`
 * installs handlers for SIGBUS, SIGILL, SIGFPE, SIGABRT and SIGSEGV so
 * that a crashed interpreter prints a Python traceback, and its OWN test
 * runner calls it before running a single test - so **every module of
 * CPython's regression suite failed on this machine at
 * `sigaction(SIGSEGV)` returning -1**, having run nothing.
 *
 * What makes it safe is not an argument, it is the two places the loop
 * is cut, and both are Unix's own:
 *
 *   - signal_deliver() blocks a signal while its own handler runs. A
 *     fault INSIDE the SIGSEGV handler therefore finds SIGSEGV blocked,
 *     finds nothing deliverable, and takes the terminate path. A handler
 *     that faults kills the process, once.
 *   - the frame is written with copy_to_user, so a fault whose cause was
 *     the stack itself cannot be reported on that stack: the write
 *     fails, the signal is dropped, and the process is terminated with
 *     the fault it actually had.
 *
 * A handler that returns without fixing anything re-executes the
 * faulting instruction and faults again, forever. That is also what
 * Linux does, it is preemptible, and it is the program's own bug. */
#define SIG_IS_CATCHABLE(sig) \
    ((sig) > 0 && (sig) <= SIG_MAX && (sig) != SIGKILL && (sig) != SIGSTOP)

/* M99: the one sa_flag the KERNEL has to know about, because it decides
 * how a handler is called rather than what it does. Everything else in
 * <signal.h>'s SA_ list is user-space policy and stays there. */
#define SA_SIGINFO 0x00000004

/* ---- M89/M99: `siginfo_t`, and who fills one in ----------------------
 *
 * M89 declared this so that a program writing a three-argument handler
 * would compile, and said plainly that nothing ever filled one in:
 * "a handler installed with SA_SIGINFO is called through sa_handler
 * with the signal number ... the pointer arguments are never passed."
 *
 * That was an accurate description of an unsafe arrangement. A handler
 * WRITTEN as three arguments and CALLED with one reads %rsi and %rdx as
 * pointers, and they contain whatever the last caller left there.
 * toybox's `timeout` installs exactly that handler for SIGCHLD and its
 * first statement is `si->si_status`; on this machine it faulted at
 * address 5 - the offset of that field added to a junk %rsi - every
 * time it ran. M99 found it while running CPython's regression suite,
 * whose fixture uses `timeout` to bound a module.
 *
 * So the kernel fills one in now, and this struct moved here from
 * <signal.h> because that makes its layout a kernel/user contract - the
 * same reason `sig_frame_t` below is here and `struct termios` is in
 * system_api at all. What each field actually holds on this machine:
 *
 *   si_signo   always the signal.
 *   si_code    CLD_EXITED or CLD_KILLED for SIGCHLD; SI_KERNEL for a
 *              fault the CPU raised; SI_USER for a signal `kill` sent.
 *   si_pid     SIGCHLD only: which child ended.
 *   si_status  SIGCHLD only: its exit code, or the signal that killed it.
 *   si_addr    SIGSEGV and SIGBUS only: the address that faulted, which
 *              is CR2 and is a fact this kernel has had all along and
 *              has never been able to tell anybody.
 *   si_errno, si_uid, si_band, si_value  zero, and zero is the answer
 *              rather than a gap: there is one principal (M65), no
 *              out-of-band data, and no realtime signals to carry a
 *              value.
 */
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
        void *sival_ptr;
    } si_value;
} siginfo_t;

/* si_code values a program tests for. */
#define SI_USER    0
#define SI_KERNEL  0x80
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

/* M85: what happens to a process that has installed no handler.
 *
 * This used to be a sentence in sched_raise_signal - "death for
 * everything here except SIGCHLD" - with a note that a table of one row
 * is a table nobody reads. Job control makes it four rows, and puts it
 * somewhere both the kernel and a program can see the same answer. */
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

/* The two well-known handler values, as every C program spells them. */
#define SIG_DFL_ADDR 0UL
#define SIG_IGN_ADDR 1UL

/* SYS_sigprocmask's `how`. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* ---- the frame a handler runs on top of --------------------------------
 *
 * When a signal is delivered, the kernel writes one of these onto the
 * target process's own stack, points RIP at the handler and RSP at the
 * word just below it - which holds the address of the restorer, so that
 * an ordinary `ret` out of an ordinary C function lands there. The
 * restorer's only job is SYS_sigreturn(frame), which puts every register
 * back exactly as it was.
 *
 * On the process's own stack rather than in the kernel, for the reason
 * every Unix does it this way: a process may be inside a signal handler
 * when the *next* signal arrives, and a single kernel-side saved context
 * would be overwritten by it. A stack nests for free.
 *
 * Laid out as a struct shared by both sides rather than as an offset the
 * assembler and the kernel each know separately - the same argument
 * system_api/README.md makes about every other structure here.
 */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, rsp;
    uint32_t saved_blocked; /* the mask to put back - the handler's own signal was added to it */
    uint32_t signo;         /* which signal this frame is for; informational, and a sanity check */
} sig_frame_t;

#ifdef __cplusplus
}
#endif
