/* user_space/libc/include/termios.h - M89
 *
 * The POSIX terminal interface, over M85's line discipline.
 *
 * The struct and the flags are not defined here: they come from
 * system_api/include/termios.h, which the kernel's line discipline
 * includes too. That is deliberate and it is the reason this header is
 * short - a `struct termios` whose layout user space and the kernel
 * disagreed about would be a program setting a flag the driver reads as
 * a different one, which is the class of bug no test finds by accident.
 *
 * What is added here is the function layer POSIX specifies and the
 * kernel does not have: tcgetattr and tcsetattr are TCGETS and TCSETS
 * through ioctl, and the rest are the calls a ported program makes that
 * this machine can answer honestly.
 *
 * **The baud-rate calls are refusals, and that is the whole reason to
 * read this comment.** cfsetispeed and friends describe a UART nothing
 * here dials; system_api's own header says defining flags for hardware
 * this machine does not have "would be the failure mode <fcntl.h> spent
 * a whole header comment avoiding". They return -1 rather than 0, so a
 * program that needs a real baud rate finds out.
 */
#pragma once

/* `struct termios`, the flags and the ioctl numbers come from
 * system_api/include/termios.h - which has the same name as this file
 * and sits later on the same include path. `#include_next` continues the
 * search from after wherever this file was found, which is exactly the
 * situation it exists for; <signal.h> in this directory has the same
 * collision with the same solution and explains it at more length. A
 * relative "../../../" path would hardcode this file's own location
 * into it. */
#include_next <termios.h>
#include <sys/types.h>

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

/* tcsetattr's `optional_actions`. All three are accepted and behave
 * identically, because this line discipline has no output queue to
 * drain and no input queue worth flushing separately - a setting takes
 * effect when it is set. Said here rather than implied: a program that
 * passes TCSADRAIN expecting output to be flushed first gets a terminal
 * whose output was never buffered in the first place. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* tcflush's queue selector, and tcflow's action. Accepted, and see
 * tcflush()'s own note in termios.c for what they actually do. */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

typedef unsigned int speed_t;

/* ---- M99: the baud rates, and the one of them that is true ----------
 *
 * The names, because CPython's Modules/termios.c lists them in a
 * constant table with no #ifdef and a system without them does not
 * degrade, it fails to compile. The values are the traditional ones.
 *
 * **Only B0 can be set on this machine**, and that is not a limitation
 * bolted on - it is M89's decision, kept, with the one case it got
 * wrong repaired. B0 on a real terminal means "no line", which is the
 * closest true statement about a console with no UART behind it, and
 * cfgetispeed has answered B0 since M89. What M89 got wrong is that
 * cfsetispeed refused *every* speed including that one, so a program
 * doing the ordinary save-and-restore - read the settings, change a
 * flag, write them back - failed on the way back with EINVAL. Python's
 * termios.tcsetattr does exactly that, which is how it was found. So
 * setting the speed this line already has now succeeds, and setting any
 * other still returns -1: a program that needs 9600 baud still finds
 * out, and a program that never wanted a baud rate is no longer told
 * about one. */
#define B0     0
#define B50    1
#define B75    2
#define B110   3
#define B134   4
#define B150   5
#define B200   6
#define B300   7
#define B600   8
#define B1200  9
#define B1800  10
#define B2400  11
#define B4800  12
#define B9600  13
#define B19200 14
#define B38400 15

int tcgetattr(int fd, struct termios *out);
int tcsetattr(int fd, int optional_actions, const struct termios *in);

/* The process group that owns the terminal - M85's TIOCGPGRP/TIOCSPGRP,
 * which is how a shell hands a job the terminal and takes it back. */
pid_t tcgetpgrp(int fd);
int   tcsetpgrp(int fd, pid_t pgrp);

int tcdrain(int fd);
int tcflush(int fd, int queue);
int tcflow(int fd, int action);
int tcsendbreak(int fd, int duration);

/* Refusals - see the header note. The two getters report 0, which is
 * B0's value and means "hung up" on a real terminal; that is the closest
 * true statement about a line with no baud rate at all. */
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int     cfsetispeed(struct termios *t, speed_t speed);
int     cfsetospeed(struct termios *t, speed_t speed);
/* Sets both, and therefore fails for both. POSIX's convenience spelling,
 * and toybox's `stty`-shaped code calls it rather than the pair. */
int     cfsetspeed(struct termios *t, speed_t speed);

/* ---- M89: cfmakeraw, which is the one of these that does something ----
 *
 * It is not a baud rate: it is the exact set of flag clears every
 * program that wants bytes-as-typed performs by hand, and every one of
 * the flags it touches that this discipline implements is honoured. So
 * unlike the four calls above this is a real operation with a real
 * effect - a terminal it has been applied to delivers ^C as a byte and
 * echoes nothing, which is what an editor asks for.
 *
 * It edits `t` and does not talk to the driver; a caller still has to
 * hand the result to tcsetattr. That is POSIX's shape and it is the
 * reason the function can be pure. */
void    cfmakeraw(struct termios *t);

#ifdef __cplusplus
}
#endif
