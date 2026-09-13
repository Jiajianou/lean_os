#include <syslog.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

static const char *log_ident;
static int log_option;
static int log_mask = 0xFF;

void openlog(const char *ident, int option, int facility) {
    (void)facility;
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
    if (len + 1 < sizeof(buf) && (len == 0 || buf[len - 1] != '\n')) {
        buf[len++] = '\n';
        buf[len] = '\0';
    }

    fputs(buf, stderr);
}

void syslog(int priority, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}
