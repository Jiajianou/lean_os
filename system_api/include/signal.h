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
    ((sig) > 0 && (sig) <= SIG_MAX && (sig) != SIGKILL && (sig) != SIGSEGV)

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
