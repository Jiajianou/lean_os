#include <sys/klog.h>

#include <errno.h>
#include <stdint.h>

#include "syscall_wrappers.h"

static uint64_t read_cursor;
static int cursor_valid;

int klogctl(int type, char *buffer, int length) {
    switch (type) {
    case SYSLOG_ACTION_READ_ALL:
    case SYSLOG_ACTION_READ_CLEAR: {
        if (!buffer || length < 0) {
            errno = EINVAL;
            return -1;
        }
        uint64_t next = 0;
        long n = sys_kernel_log(0, buffer, (size_t)length, &next);
        if (n < 0) {
            errno = EPERM;
            return -1;
        }
        read_cursor = next;
        cursor_valid = 1;
        return (int)n;
    }
    case SYSLOG_ACTION_SIZE_BUFFER: {
        long total = sys_kernel_log_total();
        return total < 0 ? 0 : (int)total;
    }
    case SYSLOG_ACTION_SIZE_UNREAD: {
        long total = sys_kernel_log_total();
        if (total < 0) {
            return 0;
        }
        if (!cursor_valid) {
            return (int)total;
        }
        return (int)((uint64_t)total > read_cursor ? (uint64_t)total - read_cursor : 0);
    }
    default:
        (void)buffer;
        (void)length;
        errno = EPERM;
        return -1;
    }
}
