/* user_space/libc/include/syslog.h - M89
 *
 * `syslog`, over the kernel log this machine already has.
 *
 * There is no syslogd here and there is not going to be one: a daemon
 * that collects messages into a file is a second logging system beside
 * M70's, which already has a ring buffer and a reader (`console`).
 *
 * **It writes to stderr.** Not to M70's log, and the reason is worth
 * stating because the opposite looks plausible: that log has no writer
 * available to user space at all. SYS_klog and SYS_klog_total are both
 * readers - the kernel log is written by the kernel - and CAP_SYSLOG
 * guards *reading* it. So a program's syslog() output goes where an
 * unprivileged program's diagnostics belong, and a program that needs it
 * collected can redirect stderr, which is the mechanism this system
 * does have.
 *
 * The priorities and facilities are the standard numbers because a
 * program computes them (`LOG_MAKEPRI`, `LOG_UPTO`) and compares them.
 * What this system does with them is narrower than the names suggest and
 * is stated in syslog.c: the facility is ignored, and the priority
 * decides only whether a message is dropped by setlogmask.
 */
#pragma once

#include <stdarg.h>

/* Priorities, most severe first - the order matters because LOG_UPTO
 * builds a mask from it. */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

#define LOG_PRIMASK 0x07
#define LOG_PRI(p)  ((p) & LOG_PRIMASK)
#define LOG_MAKEPRI(fac, pri) (((fac) << 3) | (pri))
#define LOG_MASK(pri) (1 << (pri))
#define LOG_UPTO(pri) ((1 << ((pri) + 1)) - 1)

/* Facilities. Carried and ignored - see the header note. */
#define LOG_KERN     (0 << 3)
#define LOG_USER     (1 << 3)
#define LOG_MAIL     (2 << 3)
#define LOG_DAEMON   (3 << 3)
#define LOG_AUTH     (4 << 3)
#define LOG_SYSLOG   (5 << 3)
#define LOG_LPR      (6 << 3)
#define LOG_NEWS     (7 << 3)
#define LOG_UUCP     (8 << 3)
#define LOG_CRON     (9 << 3)
#define LOG_AUTHPRIV (10 << 3)
#define LOG_FTP      (11 << 3)
#define LOG_LOCAL0   (16 << 3)
#define LOG_LOCAL1   (17 << 3)
#define LOG_LOCAL2   (18 << 3)
#define LOG_LOCAL3   (19 << 3)
#define LOG_LOCAL4   (20 << 3)
#define LOG_LOCAL5   (21 << 3)
#define LOG_LOCAL6   (22 << 3)
#define LOG_LOCAL7   (23 << 3)
#define LOG_FACMASK  0x03f8

/* openlog() options. LOG_PID and LOG_PERROR do something; the rest are
 * accepted and ignored, which is safe because each one asks for a
 * behaviour whose absence is invisible. */
#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...);
void vsyslog(int priority, const char *format, va_list ap);
void closelog(void);
int  setlogmask(int mask);
