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
 *   SIGBUS   - an unaligned or unbacked access. This kernel reports
 *              every ring-3 fault as SIGSEGV (M52), deliberately, and
 *              nothing distinguishes the two.
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
 * ignore. SIGKILL and SIGSEGV are the two that cannot be argued with -
 * one by definition, the other because its handler would run on the
 * address space that just faulted. Written as a macro rather than a
 * table so the kernel and user space cannot hold two different opinions
 * about it. */
#define SIG_IS_CATCHABLE(sig) \
    ((sig) > 0 && (sig) <= SIG_MAX && (sig) != SIGKILL && (sig) != SIGSEGV && \
     (sig) != SIGSTOP)

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
