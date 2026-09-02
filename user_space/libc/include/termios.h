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
