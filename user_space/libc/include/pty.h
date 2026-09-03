/* user_space/libc/include/pty.h - M89, filled in by M85's second attempt
 *
 * `openpty`, `forkpty`, `login_tty`, and the four `/dev/ptmx` calls
 * underneath them.
 *
 * This header shipped as three refusals. M85 made the terminal a device
 * with a line discipline and left the *pseudo* terminal out, because a
 * pty needs a path and paths needed M87's vnode layer, two milestones
 * later. That inversion is recorded in kernel/dev/pty.h; what matters
 * here is that the refusals are gone and the calls are real, over
 * kernel/dev/pty.c.
 *
 * The header exists at all because `#ifdef __APPLE__ ... #else #include
 * <pty.h>` is how a ported program decides where these live, and being
 * the `#else` case means having the file. posix_openpt, grantpt,
 * unlockpt and ptsname are declared here as well as in <stdlib.h>, where
 * the standard puts them, because a program that includes one of the two
 * should not have to know which.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

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

/* ---- the four underneath ----------------------------------------------
 *
 * posix_openpt opens /dev/ptmx, which *creates* a terminal - the one
 * open() on this machine with a side effect. ptsname is then the only
 * way to learn the slave's name, because the master descriptor does not
 * carry it. grantpt and unlockpt have nothing to do on a machine with
 * one principal and no lock bit, and return 0 having checked that their
 * argument really is a master; see the implementation for why that is a
 * true statement rather than a stub. */
int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t len);

int openpty(int *primary, int *secondary, char *name,
            const struct termios *tio, const struct winsize *ws);
pid_t forkpty(int *primary, char *name,
              const struct termios *tio, const struct winsize *ws);
int login_tty(int fd);

#ifdef __cplusplus
}
#endif
