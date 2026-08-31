/* system_api/include/termios.h - M85
 *
 * The terminal settings struct, shared by the kernel's line discipline
 * and user space's <termios.h>, so the two cannot hold different
 * opinions about what a flag means.
 *
 * Deliberately a subset, and the subset is chosen the way M63 chose
 * libc's: these are the flags a program actually reaches for. ICANON and
 * ECHO are what every "read a password" and every editor turns off; ISIG
 * is what a program turns off when it wants ^C as a character rather than
 * as an interrupt. The rest of POSIX's flag words describe hardware this
 * machine does not have - parity, modem lines, baud rates on a UART
 * nothing dials - and defining them so a program could set them would be
 * the failure mode <fcntl.h> spent a whole header comment avoiding.
 */
#pragma once

#include <stdint.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

/* c_iflag */
#define ICRNL  0x0001 /* a carriage return arrives as a newline - what Enter sends */
#define IXON   0x0002 /* accepted and ignored: there is no flow control to start or stop */

/* c_oflag */
#define OPOST  0x0001 /* post-process output: turn '\n' into CR LF on the way out */
#define ONLCR  0x0002

/* c_lflag - the three that matter */
#define ISIG   0x0001 /* ^C, ^Z and ^\ raise signals rather than arriving as bytes */
#define ICANON 0x0002 /* collect a line, allow editing, deliver it whole on Enter */
#define ECHO   0x0004 /* type it back, so a person can see what they are typing */
#define ECHOE  0x0008 /* erase visibly on backspace, rather than leaving the character */

/* Indices into c_cc. Only the ones with behaviour behind them. */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VSUSP  5
#define NCCS   8

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag; /* carried, unused - see the header note on hardware */
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

/* TIOCGWINSZ's answer. A program that draws needs to know how big the
 * terminal is, and asking is the only way that is ever right. */
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* The four ioctls this machine has, and no others. Each one exists
 * because something concrete needs it: the first two are how an editor
 * turns off canonical mode and puts it back, the third is how anything
 * that draws finds out how big the screen is, and the fourth is how a
 * shell hands the terminal to a job and takes it back. */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
