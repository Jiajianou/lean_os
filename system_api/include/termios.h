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

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

/* ---- M89: the flags a ported program names, and which ones do anything
 *
 * The note above says defining flags for hardware this machine does not
 * have "would be the failure mode <fcntl.h> spent a whole header comment
 * avoiding". That argument was about inventing a surface nobody had
 * asked for. Toybox asks for eleven of these by name - lib/tty.c's
 * set_terminal ORs CS8, CREAD, IUTF8, IXANY, ECHOK, ECHOCTL, ECHOKE and
 * IEXTEN together in one statement - so they arrive under M63's rule:
 * "when a program somebody else wrote fails to compile without a name".
 *
 * Every one added below is **carried and ignored**, and that is written
 * next to each rather than summarised, because the difference between a
 * flag with behaviour and a flag without it is the only thing a reader
 * of this file needs. A carried flag is not a lie: tcsetattr stores it,
 * tcgetattr reads it back, and the discipline's behaviour is unchanged -
 * which is exactly what a program that sets ECHOKE on a terminal with no
 * line-kill echo should observe.
 */

/* c_iflag */
#define ICRNL  0x0001 /* a carriage return arrives as a newline - what Enter sends */
#define IXON   0x0002 /* accepted and ignored: there is no flow control to start or stop */
#define IXANY  0x0004 /* carried, ignored - no flow control to unblock */
#define IXOFF  0x0008 /* carried, ignored - same */
#define IUTF8  0x0010 /* carried, ignored: erase already removes one byte, and M88's UTF-8 work is in the C library rather than in this discipline */
#define INLCR  0x0020 /* carried, ignored */
#define IGNCR  0x0040 /* carried, ignored */
#define ISTRIP 0x0080 /* carried, ignored - this console is 8-bit and always was */
#define IGNBRK 0x0100 /* carried, ignored - there is no line to break */
#define BRKINT 0x0200 /* carried, ignored - same */
#define IMAXBEL 0x0400 /* carried, ignored - the input queue drops silently */
#define IGNPAR 0x0800 /* carried, ignored - no parity on a PS/2 keyboard */
#define INPCK  0x1000 /* carried, ignored - same */
#define PARMRK 0x2000 /* carried, ignored - same */

/* c_oflag */
#define OPOST  0x0001 /* post-process output: turn '\n' into CR LF on the way out */
#define ONLCR  0x0002
#define OCRNL  0x0004 /* carried, ignored */
#define ONLRET 0x0008 /* carried, ignored */
#define ONOCR  0x0010 /* carried, ignored */

/* c_cflag - every one of these is carried and ignored, and the reason is
 * the same for all of them: there is no UART here. The console is a
 * PS/2 keyboard and a framebuffer, so character size, parity, stop bits
 * and modem control describe a line that does not exist. CS8 and CREAD
 * are in the list because toybox sets them; the rest are there so that
 * the CSIZE mask a program ANDs with means something. */
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

/* c_lflag - the three that matter */
#define ISIG   0x0001 /* ^C, ^Z and ^\ raise signals rather than arriving as bytes */
#define ICANON 0x0002 /* collect a line, allow editing, deliver it whole on Enter */
#define ECHO   0x0004 /* type it back, so a person can see what they are typing */
#define ECHOE  0x0008 /* erase visibly on backspace, rather than leaving the character */
#define ECHOK  0x0010 /* carried, ignored: VKILL erases the line on screen unconditionally */
#define ECHOKE 0x0020 /* carried, ignored - same */
#define ECHOCTL 0x0040 /* carried, ignored: a control character that is not VINTR/VQUIT/VSUSP/VERASE/VKILL/VEOF reaches the line buffer as itself and is not echoed as ^X */
#define ECHONL 0x0080 /* carried, ignored */
#define IEXTEN 0x0100 /* carried, ignored: there is no ^V quoting or ^W word erase in this discipline */
#define NOFLSH 0x0200 /* carried, ignored: a signal does not flush this queue anyway */
#define TOSTOP 0x0400 /* carried, ignored: M85 raises SIGTTOU from the write path, not from this flag */

/* Indices into c_cc. Only the ones with behaviour behind them. */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VSUSP  5
/* M89: cfmakeraw sets both, and a program that reads back what it set
 * has to find them where it put them. Carried, and the discipline does
 * not consult them: a non-canonical read here returns whatever bytes are
 * queued, which is VMIN=1/VTIME=0 behaviour and the only behaviour this
 * console has ever had. Said here rather than left to be discovered. */
#define VMIN   6
#define VTIME  7
#define NCCS   12

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
/* M89: give up the controlling terminal.
 *
 * A daemon opens /dev/tty and issues this so that a later terminal open
 * cannot become its controlling one. M85 built sessions and setsid, and
 * setsid already detaches the terminal - so on this machine this is the
 * belt to setsid's braces, and the honest thing it does is verify the
 * caller has a terminal to give up and then detach it. The number is
 * Linux's, like the four above. */
#define TIOCNOTTY  0x5422
/* M89: take this terminal as the caller's controlling one.
 *
 * The other half of TIOCNOTTY, and the call `setsid` users make right
 * after it: setsid leaves a process with a new session and no terminal,
 * and this is how it acquires one. Only a session leader may, and only
 * a terminal no other session owns - both rules are POSIX's and both
 * exist so that one session cannot steal another's terminal. */
#define TIOCSCTTY  0x540E

#ifdef __cplusplus
}
#endif
