/* user_space/libc/src/errno.c - M80 groundwork. See <errno.h>. */
#include <errno.h>

/* M96: one per THREAD, which is what this comment asked for.
 *
 * What stood here said: "One per process, not one per thread... a shared
 * errno is a real hazard in a threaded program - but thread-local
 * storage is on the 'deliberately not' list until something asks for it
 * by failing to link... the day this libc grows TLS, this variable is
 * the first thing that should move into it."
 *
 * That day is M96, and this is the move. `__thread` here is the entire
 * change: the linker puts it in .tbss, user_space/lib/user.ld gives that
 * a PT_TLS segment, and user_space/libc/src/tls.c gives every thread its
 * own copy before it runs a line of the program's code.
 *
 * The hazard it closes is specific and is worth naming: two threads
 * whose syscalls interleave used to overwrite each other's errno between
 * the failing call and the check of it, so a thread could read a
 * diagnosis of somebody else's failure - or of its own success. That is
 * not a race a program can defend against. */
/* The variable itself, under a name nothing outside this file uses -
 * `errno` is the macro in <errno.h> and expands to a call. */
static __thread int lean_errno;

int *__errno_location(void) {
    return &lean_errno;
}
