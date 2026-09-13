#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/utsname.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "process.h"
#include "syscall_wrappers.h"

static void field(char *destination, const char *source) {
    size_t i = 0;
    for (; source[i] && i < _UTSNAME_LENGTH - 1; i++) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

int uname(struct utsname *buffer) {
    if (!buffer) {
        errno = EFAULT;
        return -1;
    }
    memset(buffer, 0, sizeof(*buffer));
    field(buffer->sysname, "lean_os");
    field(buffer->nodename, "");
    field(buffer->release, "0.89");
    field(buffer->version, "lean_os M89");
    field(buffer->machine, "x86_64");
    field(buffer->domainname, "");
    return 0;
}

int sched_yield(void) {
    sys_yield();
    return 0;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *parameter) {
    (void)pid;
    (void)policy;
    (void)parameter;
    errno = ENOSYS;
    return -1;
}

int sched_getscheduler(pid_t pid) {
    (void)pid;
    return SCHED_OTHER;
}

int sched_get_priority_min(int policy) {
    (void)policy;
    return 0;
}

int sched_get_priority_max(int policy) {
    (void)policy;
    return 0;
}

ssize_t readv(int fd, const struct iovec *iov, int count) {
    if (!iov || count < 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return total > 0 ? total : -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int count) {
    if (!iov || count < 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            return total > 0 ? total : -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

#include <sys/mount.h>

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    (void)source;
    (void)target;
    (void)fstype;
    (void)flags;
    (void)data;
    errno = ENOSYS;
    return -1;
}

int umount(const char *target) {
    (void)target;
    errno = ENOSYS;
    return -1;
}

int umount2(const char *target, int flags) {
    (void)target;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

#include <sys/syscall.h>

long syscall(long number, ...) {
    (void)number;
    errno = ENOSYS;
    return -1;
}

int sysinfo(struct sysinfo *info) {
    if (!info) {
        errno = EFAULT;
        return -1;
    }
    memset(info, 0, sizeof(*info));

    info->uptime = (long)(sys_uptime_ms() / 1000);

    os_meminfo_t mi;
    if (sys_meminfo(&mi) == 0) {
        info->mem_unit = (unsigned int)mi.page_size;
        info->totalram = (unsigned long)mi.total_frames;
        info->freeram = (unsigned long)mi.free_frames;
    } else {
        info->mem_unit = 4096;
    }

    long n = sys_taskinfo((task_info_t *)0, 0);
    if (n > 0) {
        info->procs = (unsigned short)n;
    }
    return 0;
}
