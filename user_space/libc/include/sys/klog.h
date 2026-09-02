/* user_space/libc/include/sys/klog.h - M89
 *
 * The kernel log, read through the interface `dmesg` expects.
 *
 * **This one is real.** M70 built a kernel log with a ring buffer and
 * SYS_klog to read it back, precisely because "this kernel's entire
 * diagnostic surface was a black screen" - and the program in the world
 * whose whole job is to print that log calls klogctl(). So this header
 * is a translation between two spellings of a thing that exists, not a
 * declaration of one that does not.
 *
 * The actions, and which are answered:
 *
 *   3  SYSLOG_ACTION_READ_ALL   - real: the whole ring, oldest first
 *   4  SYSLOG_ACTION_READ_CLEAR - the read half is real; nothing is
 *                                 cleared, see below
 *   10 SYSLOG_ACTION_SIZE_BUFFER - real: how big the ring is
 *   9  SYSLOG_ACTION_SIZE_UNREAD - real: bytes logged since this process
 *                                 last read, which is what SYS_klog's
 *                                 cursor already tracks
 *   5  SYSLOG_ACTION_CLEAR      - refused (EPERM)
 *   6/7/8 console level control - refused (EPERM)
 *   0/1/2 open/close/read-one   - refused (EPERM)
 *
 * **Why clearing is refused rather than ignored.** The log is a ring the
 * kernel writes into; nothing can remove entries from it, and a `dmesg
 * -C` that returned success would tell an operator the log was empty
 * when the next boot marker would prove otherwise. EPERM is the error
 * `dmesg -C` already handles - it is what an unprivileged user gets on
 * Linux.
 *
 * Reading needs CAP_SYSLOG (M65): the log describes what every process
 * on the machine is doing. A program without it gets -1 and EPERM, which
 * is the same answer and the same reason.
 */
#pragma once

#include <stddef.h>

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

#define SYSLOG_ACTION_CLOSE        0
#define SYSLOG_ACTION_OPEN         1
#define SYSLOG_ACTION_READ         2
#define SYSLOG_ACTION_READ_ALL     3
#define SYSLOG_ACTION_READ_CLEAR   4
#define SYSLOG_ACTION_CLEAR        5
#define SYSLOG_ACTION_CONSOLE_OFF  6
#define SYSLOG_ACTION_CONSOLE_ON   7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD  9
#define SYSLOG_ACTION_SIZE_BUFFER  10

int klogctl(int type, char *buf, int len);

#ifdef __cplusplus
}
#endif
