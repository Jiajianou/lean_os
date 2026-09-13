#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "os_poll.h"
#include "syscall_wrappers.h"

int epoll_create(int size) {
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return epoll_create1(0);
}

int epoll_create1(int flags) {
    long fd = sys_epoll_create(flags);
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    return (int)fd;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    _Static_assert(sizeof(struct epoll_event) == sizeof(os_epoll_event_t),
                   "struct epoll_event must be os_poll.h's os_epoll_event_t");
    _Static_assert(sizeof(((struct epoll_event *)0)->data) == sizeof(uint64_t),
                   "epoll_data_t must be eight bytes");
    if (op != EPOLL_CTL_DEL && !event) {
        errno = EFAULT;
        return -1;
    }
    os_epoll_event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (event) {
        ev.events = event->events;
        memcpy(&ev.data, &event->data, sizeof(ev.data));
    }
    if (sys_epoll_ctl(epfd, op, fd, &ev) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (!events || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }
    long n = sys_epoll_wait(epfd, (os_epoll_event_t *)events, maxevents, timeout);
    if (n == -OS_ERR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (n < 0) {
        errno = EINVAL;
        return -1;
    }
    return (int)n;
}

int eventfd(unsigned int initval, int flags) {
    long fd = sys_eventfd(initval, flags);
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    return (int)fd;
}

int eventfd_read(int fd, eventfd_t *value) {
    eventfd_t v = 0;
    ssize_t n = read(fd, &v, sizeof(v));
    if (n != (ssize_t)sizeof(v)) {
        return -1;
    }
    if (value) {
        *value = v;
    }
    return 0;
}

int eventfd_write(int fd, eventfd_t value) {
    return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) ? 0 : -1;
}

static uint64_t to_ns(const struct timespec *ts) {
    if (!ts) {
        return 0;
    }
    if (ts->tv_sec < 0 || ts->tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static void from_ns(struct timespec *ts, uint64_t ns) {
    if (!ts) {
        return;
    }
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

int timerfd_create(int clockid, int flags) {
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    long fd = sys_timerfd_create(clockid, flags);
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    return (int)fd;
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value) {
    if (!new_value) {
        errno = EFAULT;
        return -1;
    }
    if (new_value->it_value.tv_nsec < 0 || new_value->it_value.tv_nsec >= 1000000000L ||
        new_value->it_interval.tv_nsec < 0 || new_value->it_interval.tv_nsec >= 1000000000L ||
        new_value->it_value.tv_sec < 0 || new_value->it_interval.tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    os_itimer_t want;
    want.value_ns = to_ns(&new_value->it_value);
    want.interval_ns = to_ns(&new_value->it_interval);
    os_itimer_t had = {0, 0};
    if (sys_timerfd_settime(fd, flags, &want, old_value ? &had : (os_itimer_t *)0) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (old_value) {
        from_ns(&old_value->it_value, had.value_ns);
        from_ns(&old_value->it_interval, had.interval_ns);
    }
    return 0;
}

int timerfd_gettime(int fd, struct itimerspec *curr_value) {
    if (!curr_value) {
        errno = EFAULT;
        return -1;
    }
    os_itimer_t out = {0, 0};
    if (sys_timerfd_gettime(fd, &out) != 0) {
        errno = EINVAL;
        return -1;
    }
    from_ns(&curr_value->it_value, out.value_ns);
    from_ns(&curr_value->it_interval, out.interval_ns);
    return 0;
}
