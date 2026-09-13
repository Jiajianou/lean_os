#pragma once

#include_next <termios.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

typedef unsigned int speed_t;

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

pid_t tcgetpgrp(int fd);
int   tcsetpgrp(int fd, pid_t pgrp);

int tcdrain(int fd);
int tcflush(int fd, int queue);
int tcflow(int fd, int action);
int tcsendbreak(int fd, int duration);

speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int     cfsetispeed(struct termios *t, speed_t speed);
int     cfsetospeed(struct termios *t, speed_t speed);
int     cfsetspeed(struct termios *t, speed_t speed);

void    cfmakeraw(struct termios *t);

#ifdef __cplusplus
}
#endif
