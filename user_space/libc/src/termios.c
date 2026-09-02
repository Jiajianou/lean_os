/* user_space/libc/src/termios.c - M89
 *
 * The POSIX terminal calls over M85's ioctls. See <termios.h> for why
 * the struct is system_api's rather than this file's, and for why the
 * baud-rate calls refuse.
 */
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdarg.h>

#include "syscall_wrappers.h"

int tcgetattr(int fd, struct termios *out) {
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    if (sys_ioctl(fd, TCGETS, out) != 0) {
        /* The kernel refuses this for anything that is not a terminal,
         * which is what ENOTTY means and is the errno every caller of
         * isatty-by-tcgetattr is testing for. */
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *in) {
    /* All three actions behave identically, and <termios.h> says why:
     * there is no output queue to drain. Accepted rather than refused,
     * because a program passing TCSADRAIN is asking for its setting to
     * take effect after pending output - and on a terminal with no
     * buffered output that is the same instant as TCSANOW. */
    (void)optional_actions;
    if (!in) {
        errno = EFAULT;
        return -1;
    }
    if (sys_ioctl(fd, TCSETS, (void *)in) != 0) {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

pid_t tcgetpgrp(int fd) {
    int pgrp = 0;
    if (sys_ioctl(fd, TIOCGPGRP, &pgrp) != 0) {
        errno = ENOTTY;
        return (pid_t)-1;
    }
    return (pid_t)pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp) {
    int v = (int)pgrp;
    if (sys_ioctl(fd, TIOCSPGRP, &v) != 0) {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

/* ---- the three that have nothing to do ------------------------------
 *
 * A success that does nothing, and unlike the baud-rate calls below that
 * is the honest answer rather than a convenient one. tcdrain waits for
 * queued output to be written: this line discipline writes synchronously,
 * so by the time a caller can ask, there is nothing queued and the wait
 * is already over. Returning -1 would make a program think it could not
 * do something that has in fact already happened.
 *
 * tcflush is the one with a real gap, and it is worth naming: it should
 * discard unread input, and this kernel's tty has no call to do that. A
 * program calling tcflush(TCIFLUSH) to drop type-ahead before a prompt
 * will still see the type-ahead. That is a missing feature reported as a
 * success, which is the shape this project usually refuses - it is
 * accepted here because the alternative fails a program that would
 * otherwise work correctly except for stale input, and because the fix
 * is a kernel ioctl rather than anything this file can do. It is written
 * down so the next person meets it here rather than in a debugger.
 */
int tcdrain(int fd) {
    (void)fd;
    return 0;
}

int tcflush(int fd, int queue) {
    (void)fd;
    (void)queue;
    return 0;
}

int tcflow(int fd, int action) {
    (void)fd;
    (void)action;
    return 0; /* there is no flow control to suspend or resume - see <termios.h> */
}

int tcsendbreak(int fd, int duration) {
    (void)fd;
    (void)duration;
    return 0; /* a break is a condition on a serial line this terminal is not */
}

/* ---- the refusals ---------------------------------------------------- */

speed_t cfgetispeed(const struct termios *t) {
    (void)t;
    /* 0 is B0, which on a real terminal means "hung up". It is the
     * closest true statement about a line that has no baud rate at all -
     * and a program that treats 0 as an error will take its error path,
     * which is better than one that believes it got 9600. */
    return 0;
}

speed_t cfgetospeed(const struct termios *t) {
    (void)t;
    return 0;
}

int cfsetispeed(struct termios *t, speed_t speed) {
    (void)t;
    (void)speed;
    errno = EINVAL;
    return -1;
}

int cfsetospeed(struct termios *t, speed_t speed) {
    (void)t;
    (void)speed;
    errno = EINVAL;
    return -1;
}

int cfsetspeed(struct termios *t, speed_t speed) {
    (void)t;
    (void)speed;
    errno = EINVAL;
    return -1;
}

/* ---- and the one that is not a refusal -------------------------------
 *
 * cfmakeraw is the flag arithmetic every program that wants bytes as
 * typed writes by hand, and every flag it clears that this discipline
 * implements is genuinely honoured afterwards: ICANON, ECHO, ECHOE, ISIG
 * and ICRNL are the five kernel/dev/tty.c reads, and clearing them is
 * exactly what makes ^C arrive as byte 3.
 *
 * The rest of what it touches is carried and ignored, which changes
 * nothing about the result - a discipline that never stripped a parity
 * bit does not start behaving differently when ISTRIP is cleared.
 */
void cfmakeraw(struct termios *t) {
    if (!t) {
        return;
    }
    t->c_iflag &= ~(unsigned)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                              IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(unsigned)OPOST;
    t->c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(unsigned)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

/* ---- M89: ioctl itself ----------------------------------------------
 *
 * <sys/ioctl.h> has declared this since M89's header pass and nothing
 * implemented it - every caller in this tree went through sys_ioctl or
 * through tcgetattr above. A program written elsewhere calls `ioctl`.
 *
 * It lives in this file rather than in one of its own because every
 * request this kernel implements is a terminal request (see
 * system_api/include/termios.h), so this is where the reader who wants
 * to know what an ioctl can do here is already looking.
 *
 * The variadic third argument is always taken as a pointer, which is
 * what all six commands take. An ioctl the kernel does not know returns
 * -1 with ENOTTY - the error that means "this descriptor does not
 * support that request", which is exactly the situation and is what a
 * caller probing for a capability tests for.
 */
int ioctl(int fd, unsigned long request, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, request);
    void *arg = __builtin_va_arg(ap, void *);
    __builtin_va_end(ap);
    if (sys_ioctl(fd, (unsigned int)request, arg) != 0) {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}
