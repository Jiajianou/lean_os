#pragma once

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The register order below is glibc's, and it is a contract rather than a
   preference - the same kind as the C names in this directory. Every program
   that reads a ucontext on x86-64 indexes gregs with these names, and a
   profiler that walked a differently ordered array would not fail, it would
   report the wrong stack. */
#define REG_R8      0
#define REG_R9      1
#define REG_R10     2
#define REG_R11     3
#define REG_R12     4
#define REG_R13     5
#define REG_R14     6
#define REG_R15     7
#define REG_RDI     8
#define REG_RSI     9
#define REG_RBP    10
#define REG_RBX    11
#define REG_RDX    12
#define REG_RAX    13
#define REG_RCX    14
#define REG_RSP    15
#define REG_RIP    16
#define REG_EFL    17
#define REG_CSGSFS 18
#define REG_ERR    19
#define REG_TRAPNO 20
#define REG_OLDMASK 21
#define REG_CR2    22

#define NGREG 23

typedef long long greg_t;
typedef greg_t gregset_t[NGREG];

/* fpregs is a null pointer here and says so: this kernel saves the x87 and
   SSE state with fxsave into the task, not onto the signal stack, so there
   is nothing at a user address for a handler to point at. A handler that
   needs those registers is the condition for changing it. */
typedef struct {
    gregset_t gregs;
    void *fpregs;
    unsigned long long __reserved[8];
} mcontext_t;

typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
} ucontext_t;

#ifdef __cplusplus
}
#endif
