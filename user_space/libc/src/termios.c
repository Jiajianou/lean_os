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
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *in) {
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
    return 0;
}

int tcsendbreak(int fd, int duration) {
    (void)fd;
    (void)duration;
    return 0;
}

speed_t cfgetispeed(const struct termios *t) {
    (void)t;
    return 0;
}

speed_t cfgetospeed(const struct termios *t) {
    (void)t;
    return 0;
}

static int set_speed(speed_t speed) {
    if (speed == B0) {
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int cfsetispeed(struct termios *t, speed_t speed) {
    (void)t;
    return set_speed(speed);
}

int cfsetospeed(struct termios *t, speed_t speed) {
    (void)t;
    return set_speed(speed);
}

int cfsetspeed(struct termios *t, speed_t speed) {
    (void)t;
    return set_speed(speed);
}

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
