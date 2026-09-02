/* user_space/libc/src/klog.c - M89
 *
 * klogctl over SYS_klog. See <sys/klog.h> for which actions are answered
 * and, more usefully, why the two that are not are refusals rather than
 * successes.
 */
#include <sys/klog.h>

#include <errno.h>
#include <stdint.h>

#include "syscall_wrappers.h"

/* Where this process has read up to. SYS_klog takes an absolute byte
 * position and hands back where to resume - that cursor is what makes
 * SIZE_UNREAD answerable, and it is per-process because "unread" is a
 * statement about a reader rather than about the log. */
static uint64_t read_cursor;
static int cursor_valid;

int klogctl(int type, char *buf, int len) {
    switch (type) {
    case SYSLOG_ACTION_READ_ALL:
    case SYSLOG_ACTION_READ_CLEAR: {
        if (!buf || len < 0) {
            errno = EINVAL;
            return -1;
        }
        uint64_t next = 0;
        long n = sys_klog(0, buf, (size_t)len, &next);
        if (n < 0) {
            errno = EPERM; /* CAP_SYSLOG - see <sys/klog.h> */
            return -1;
        }
        read_cursor = next;
        cursor_valid = 1;
        /* READ_CLEAR reads and does not clear. The difference from
         * READ_ALL is therefore nothing, which is stated in the header
         * rather than hidden here - a caller passing 4 gets its bytes. */
        return (int)n;
    }
    case SYSLOG_ACTION_SIZE_BUFFER: {
        /* How much there is to read, which is what dmesg asks before
         * allocating. The total ever logged is the upper bound and the
         * ring is smaller, so this reports the total: a buffer that is
         * too big wastes memory for one command, and one that is too
         * small truncates the log. */
        long total = sys_klog_total();
        return total < 0 ? 0 : (int)total;
    }
    case SYSLOG_ACTION_SIZE_UNREAD: {
        long total = sys_klog_total();
        if (total < 0) {
            return 0;
        }
        if (!cursor_valid) {
            return (int)total;
        }
        return (int)((uint64_t)total > read_cursor ? (uint64_t)total - read_cursor : 0);
    }
    default:
        (void)buf;
        (void)len;
        errno = EPERM;
        return -1;
    }
}
