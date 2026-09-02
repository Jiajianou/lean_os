/* user_space/libc/include/sys/ttydefaults.h - M89
 *
 * The default control characters a terminal starts with.
 *
 * These are the values M85's line discipline actually uses - ^C to
 * interrupt, ^\ to quit, ^H to erase, ^U to kill the line, ^D for end of
 * file, ^Z to suspend - so a program that prints "type ^D to finish" is
 * printing something true here.
 *
 * CTRL() is the macro that makes the table readable and is what the
 * header is really for: every program that resets a terminal writes
 * CTRL('c') rather than 3.
 */
#pragma once

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

#define CTRL(x) ((x) & 0x1F)

#define CEOF     CTRL('d')
#define CERASE   0x7F        /* DEL, which is what a modern keyboard's Backspace sends */
#define CINTR    CTRL('c')
#define CKILL    CTRL('u')
#define CQUIT    CTRL('\\')
#define CSUSP    CTRL('z')
#define CSTART   CTRL('q')
#define CSTOP    CTRL('s')
#define CEOL     0
#define CMIN     1
#define CTIME    0
#define CWERASE  CTRL('w')
#define CLNEXT   CTRL('v')
#define CREPRINT CTRL('r')
#define CDISCARD CTRL('o')
#define CEOT     CEOF
#define CBRK     CEOL
#define CRPRNT   CREPRINT
#define CFLUSH   CDISCARD

#ifdef __cplusplus
}
#endif
