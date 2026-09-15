#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* POSIX per-process timers, on M119's timerfd and one thread apiece.
 *
 * The thread is the cost, and it is here because POSIX says a timer's
 * expiry NOTIFIES - it raises a signal or calls a function - and a
 * descriptor does not notify anybody, it just becomes readable. Something
 * has to be waiting on it. This kernel has no path that raises a signal
 * from a timer, and adding one would be a second timer mechanism beside
 * the one M119 built; a thread blocked in read() is the same wait, in the
 * place a program can see it.
 *
 * What that buys is that the overrun count is exact rather than estimated:
 * a timerfd read returns the number of expirations since the last one,
 * which is what timer_getoverrun is defined to report.
 */

struct __timer {
    int descriptor;
    int notify;
    int signal_number;
    union sigval value;
    void (*function)(union sigval);
    pthread_t thread;
    volatile int running;
    volatile int overrun;
    volatile int armed;
};

static void *timer_thread(void *argument) {
    struct __timer *timer = (struct __timer *)argument;
    while (timer->running) {
        unsigned long long expirations = 0;
        ssize_t got = read(timer->descriptor, &expirations, sizeof(expirations));
        if (!timer->running) {
            break;
        }
        if (got != (ssize_t)sizeof(expirations) || expirations == 0) {
            continue;
        }
        /* POSIX counts the expirations BEYOND the one being delivered. */
        timer->overrun = (int)(expirations - 1);
        if (timer->notify == SIGEV_SIGNAL) {
            raise(timer->signal_number);
        } else if (timer->notify == SIGEV_THREAD && timer->function) {
            timer->function(timer->value);
        }
    }
    return (void *)0;
}

int timer_create(clockid_t clock, struct sigevent *notification, timer_t *out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }

    struct __timer *timer = calloc(1, sizeof(*timer));
    if (!timer) {
        errno = EAGAIN;
        return -1;
    }

    timer->notify = notification ? notification->sigev_notify : SIGEV_SIGNAL;
    timer->signal_number = notification ? notification->sigev_signo : SIGALRM;
    if (notification) {
        timer->value = notification->sigev_value;
        timer->function = notification->sigev_notify_function;
    } else {
        timer->value.sival_ptr = (void *)0;
        timer->function = (void (*)(union sigval))0;
    }
    if (timer->notify != SIGEV_SIGNAL && timer->notify != SIGEV_NONE &&
        timer->notify != SIGEV_THREAD) {
        free(timer);
        errno = EINVAL;
        return -1;
    }

    timer->descriptor = timerfd_create(clock, 0);
    if (timer->descriptor < 0) {
        free(timer);
        errno = EAGAIN;
        return -1;
    }

    /* SIGEV_NONE has nobody to notify, so it gets no thread - the timer is
       then only ever read through timer_gettime, which is exactly what that
       notification means. */
    if (timer->notify != SIGEV_NONE) {
        timer->running = 1;
        if (pthread_create(&timer->thread, (pthread_attr_t *)0, timer_thread,
                           timer) != 0) {
            close(timer->descriptor);
            free(timer);
            errno = EAGAIN;
            return -1;
        }
    }

    *out = timer;
    return 0;
}

int timer_delete(timer_t timer) {
    if (!timer) {
        errno = EINVAL;
        return -1;
    }
    if (timer->running) {
        /* Disarm first, then let the blocked read fail when the descriptor
           closes - there is no way to interrupt a read on this OS and a
           timer that never fires again is one the thread stops waiting for
           the moment its descriptor goes. */
        struct itimerspec stop;
        memset(&stop, 0, sizeof(stop));
        timerfd_settime(timer->descriptor, 0, &stop, (struct itimerspec *)0);
        timer->running = 0;
        close(timer->descriptor);
        pthread_detach(timer->thread);
    } else {
        close(timer->descriptor);
    }
    free(timer);
    return 0;
}

int timer_settime(timer_t timer, int flags, const struct itimerspec *value,
                  struct itimerspec *previous) {
    if (!timer || !value) {
        errno = EINVAL;
        return -1;
    }
    int timerfd_flags = (flags & TIMER_ABSTIME) ? TFD_TIMER_ABSTIME : 0;
    if (timerfd_settime(timer->descriptor, timerfd_flags, value, previous) != 0) {
        return -1;
    }
    timer->armed = value->it_value.tv_sec != 0 || value->it_value.tv_nsec != 0;
    timer->overrun = 0;
    return 0;
}

int timer_gettime(timer_t timer, struct itimerspec *out) {
    if (!timer || !out) {
        errno = EINVAL;
        return -1;
    }
    return timerfd_gettime(timer->descriptor, out);
}

int timer_getoverrun(timer_t timer) {
    if (!timer) {
        errno = EINVAL;
        return -1;
    }
    return timer->overrun;
}
