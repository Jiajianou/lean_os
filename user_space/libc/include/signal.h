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

/* Block/unblock/replace this process's blocked set. `mask` is a plain
 * 32-bit word with bit N meaning signal N, not POSIX's opaque sigset_t -
 * see system_api/include/signal.h for why. `old` may be NULL. */
int sigprocmask(int how, unsigned int mask, unsigned int *old);
