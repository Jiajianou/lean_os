/* user_space/libc/include/pty.h - M89
 *
 * `openpty`, `forkpty`, `login_tty` - and there are no pseudo-terminals
 * on this machine, so all three are refusals.
 *
 * That is not an oversight and it has a date. M85 made the terminal a
 * device with a line discipline and deliberately left the *pseudo*
 * terminal out; the arc note in milestones.md puts it in M98, where an
 * hour-long compiler build needs `^C` to reach the thing it is building.
 * Until then a program that wants to allocate a terminal for a child
 * gets -1 and ENOSYS, which is what "this system cannot do that" looks
 * like - as opposed to a stub returning a file descriptor that is not a
 * terminal, which is how a program ends up reporting that its child
 * exited for no reason.
 *
 * The header exists at all because `#ifdef __APPLE__ ... #else #include
 * <pty.h>` is how a ported program decides where these live, and being
 * the `#else` case means having the file.
 */
#pragma once

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

int openpty(int *primary, int *secondary, char *name,
            const struct termios *tio, const struct winsize *ws);
pid_t forkpty(int *primary, char *name,
              const struct termios *tio, const struct winsize *ws);
int login_tty(int fd);

#ifdef __cplusplus
}
#endif
