#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

#define ICRNL  0x0001
#define IXON   0x0002
#define IXANY  0x0004
#define IXOFF  0x0008
#define IUTF8  0x0010
#define INLCR  0x0020
#define IGNCR  0x0040
#define ISTRIP 0x0080
#define IGNBRK 0x0100
#define BRKINT 0x0200
#define IMAXBEL 0x0400
#define IGNPAR 0x0800
#define INPCK  0x1000
#define PARMRK 0x2000

#define OPOST  0x0001
#define ONLCR  0x0002
#define OCRNL  0x0004
#define ONLRET 0x0008
#define ONOCR  0x0010

#define CS5    0x0000
#define CS6    0x0010
#define CS7    0x0020
#define CS8    0x0030
#define CSIZE  0x0030
#define CSTOPB 0x0040
#define CREAD  0x0080
#define PARENB 0x0100
#define PARODD 0x0200
#define HUPCL  0x0400
#define CLOCAL 0x0800

#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0004
#define ECHOE  0x0008
#define ECHOK  0x0010
#define ECHOKE 0x0020
#define ECHOCTL 0x0040
#define ECHONL 0x0080
#define IEXTEN 0x0100
#define NOFLSH 0x0200
#define TOSTOP 0x0400

#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VSUSP  5
#define VMIN   6
#define VTIME  7
#define VSTART 8
#define VSTOP  9
#define VEOL   10
#define VEOL2  11
#define NCCS   12

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCNOTTY  0x5422
#define TIOCSCTTY  0x540E

#define TIOCGPTN   0x80045430
#define TIOCSWINSZ 0x5414

#ifdef __cplusplus
}
#endif
