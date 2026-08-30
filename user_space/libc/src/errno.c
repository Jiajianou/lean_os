/* user_space/libc/src/errno.c - M80 groundwork. See <errno.h>. */
#include <errno.h>

/* One per process, not one per thread. M79 gave this machine threads
 * that share an address space, and a shared errno is a real hazard in a
 * threaded program - but thread-local storage is on the "deliberately
 * not" list until something asks for it by failing to link, and a
 * per-thread errno needs it. Written down here rather than left to be
 * discovered: the day this libc grows TLS, this variable is the first
 * thing that should move into it. */
int errno;
