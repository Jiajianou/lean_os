/* user_space/libc/include/sys/ioctl.h - M89
 *
 * `ioctl`, and the five commands this machine actually implements.
 *
 * The commands come from system_api/include/termios.h so there is one
 * definition of each number rather than two. What this header adds is
 * the prototype, in the place a ported program includes to get it.
 *
 * An unrecognised command is -1 from the kernel, not a silent success.
 * That matters more here than for most calls: ioctl is the syscall
 * programs use to ask about hardware, and a kernel that returned 0 for
 * an ioctl it did not understand would have a program believe it had
 * configured something.
 */
#pragma once

#include "termios.h" /* system_api's - TCGETS, TCSETS, TIOCGWINSZ, TIOCGPGRP, TIOCSPGRP, struct winsize */

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

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
