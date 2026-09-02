/* user_space/libc/src/syslog.c - M89
 *
 * syslog() over M70's kernel log. See <syslog.h> for why there is no
 * daemon and no second log.
 */
#include <syslog.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

static const char *log_ident;
static int log_option;
static int log_mask = 0xFF; /* everything, until setlogmask says otherwise */

void openlog(const char *ident, int option, int facility) {
    (void)facility; /* one log, so a facility selects nothing */
    log_ident = ident;
    log_option = option;
}

void closelog(void) {
    log_ident = (const char *)0;
    log_option = 0;
}

int setlogmask(int mask) {
    int old = log_mask;
    if (mask != 0) {
        log_mask = mask;
    }
    return old;
}

void vsyslog(int priority, const char *format, va_list ap) {
    if (!(log_mask & LOG_MASK(LOG_PRI(priority)))) {
        return;
    }
    char buf[512];
    int n = 0;
    if (log_ident) {
        n = snprintf(buf, sizeof(buf), "%s", log_ident);
        if (log_option & LOG_PID) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "[%d]", (int)sys_getpid());
        }
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, ": ");
    }
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return;
    }
    int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, format, ap);
    if (m < 0) {
        return;
    }
    size_t len = strlen(buf);
    /* A trailing newline, because the kernel log is a stream of lines
     * and syslog()'s callers conventionally do not supply one. */
    if (len + 1 < sizeof(buf) && (len == 0 || buf[len - 1] != '\n')) {
        buf[len++] = '\n';
        buf[len] = '\0';
    }

    /* stderr, and only stderr - see <syslog.h>.
     *
     * The first version of this routed to M70's kernel log when the
     * caller held CAP_SYSLOG. It does not, because there is no syscall
     * that writes there: SYS_klog and SYS_klog_total are both readers,
     * and the kernel log is written by the kernel. Writing this file
     * against a call that does not exist was caught by the linker, which
     * is the right place for it to be caught and is worth recording -
     * the capability existing (`console` holds CAP_SYSLOG) made a write
     * path look plausible when only a read path is there. */
    fputs(buf, stderr);
}

void syslog(int priority, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}
