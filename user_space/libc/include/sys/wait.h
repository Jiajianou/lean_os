/* user_space/libc/include/sys/wait.h - M84
 *
 * The header every program that forks reaches for next.
 *
 * SYS_wait has existed since M13 and returns an exit code, which sounds
 * like enough until you notice what it cannot say: a program that calls
 * `exit(139)` and one killed by SIGSEGV both produce 139, because
 * 128 + signal is exactly the convention that makes a signal death
 * readable to a person - and therefore ambiguous to a program. This
 * header is the other answer.
 *
 * The status encoding is the familiar one and is familiar deliberately:
 * a normal exit is the code in bits 8-15 with the low seven bits clear,
 * a death by signal is the signal in the low seven bits. Programs
 * written elsewhere test these macros, not the layout, but the layout is
 * what makes the macros they were compiled against work.
 *
 * Deliberately not here: WUNTRACED, WCONTINUED, WIFSTOPPED. Those are
 * about job control - a stopped process is one a terminal suspended -
 * and nothing on this machine can stop a process yet. M85 is where
 * SIGTSTP arrives, and it is where these belong.
 */
#pragma once

#include <sys/types.h>

/* Return immediately with 0 if no child has exited, rather than waiting.
 * The difference between a shell that can report a background job and one
 * that stops to wait for it. */
#define WNOHANG 1

#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0)
#define WTERMSIG(status)    ((status) & 0x7F)

/* Reaps a terminated child and reports which and how.
 *
 * `pid` is -1 for any child, or a specific child's pid. Returns the pid
 * reaped, 0 when WNOHANG was given and the child is still alive, and -1
 * if there is no such child to wait for. `status` may be NULL if the
 * caller only wants to know that the child is gone.
 *
 * A negative pid other than -1 - the "any child in this process group"
 * form - is refused rather than quietly treated as -1, because process
 * groups do not mean anything here yet. */
pid_t waitpid(pid_t pid, int *status, int options);

/* waitpid(-1, status, 0), which is all `wait` has ever been. */
pid_t wait(int *status);
