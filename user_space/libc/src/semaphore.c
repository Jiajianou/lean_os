#include <errno.h>
#include <limits.h>
#include <semaphore.h>
#include <time.h>

#include <process.h>

#include "syscall_wrappers.h"

static void wait_on(volatile int *address, int observed, unsigned int timeout_ms) {
    sys_futex((volatile unsigned int *)address, FUTEX_WAIT,
              (unsigned int)observed, timeout_ms);
}

static void wake_one(volatile int *address) {
    sys_futex((volatile unsigned int *)address, FUTEX_WAKE, 1, 0);
}

int sem_init(sem_t *semaphore, int shared, unsigned int value) {
    if (!semaphore || value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    /* A semaphore in shared memory works here without being told so - it is
       two integers and a futex on their address, and this kernel's futex
       keys on the physical page. The argument is accepted rather than
       refused for that reason, not ignored. */
    (void)shared;
    semaphore->value = (int)value;
    semaphore->waiters = 0;
    return 0;
}

int sem_destroy(sem_t *semaphore) {
    if (!semaphore) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sem_trywait(sem_t *semaphore) {
    if (!semaphore) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        int seen = semaphore->value;
        if (seen <= 0) {
            errno = EAGAIN;
            return -1;
        }
        if (__atomic_compare_exchange_n(&semaphore->value, &seen, seen - 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
    }
}

static int wait_until(sem_t *semaphore, const struct timespec *deadline) {
    for (;;) {
        if (sem_trywait(semaphore) == 0) {
            return 0;
        }
        unsigned int timeout_ms = 0;
        if (deadline) {
            struct timespec now;
            if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
                errno = EINVAL;
                return -1;
            }
            long long left_ns = ((long long)deadline->tv_sec - now.tv_sec) * 1000000000LL +
                                ((long long)deadline->tv_nsec - now.tv_nsec);
            if (left_ns <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            long long left = (left_ns + 999999LL) / 1000000LL;
            timeout_ms = left > 0x7FFFFFFF ? 0x7FFFFFFFu : (unsigned int)left;
        }
        __atomic_fetch_add(&semaphore->waiters, 1, __ATOMIC_RELAXED);
        if (semaphore->value <= 0) {
            wait_on(&semaphore->value, 0, timeout_ms);
        }
        __atomic_fetch_sub(&semaphore->waiters, 1, __ATOMIC_RELAXED);
    }
}

int sem_wait(sem_t *semaphore) {
    if (!semaphore) {
        errno = EINVAL;
        return -1;
    }
    return wait_until(semaphore, 0);
}

int sem_timedwait(sem_t *semaphore, const struct timespec *deadline) {
    if (!semaphore || !deadline) {
        errno = EINVAL;
        return -1;
    }
    return wait_until(semaphore, deadline);
}

int sem_post(sem_t *semaphore) {
    if (!semaphore) {
        errno = EINVAL;
        return -1;
    }
    if (semaphore->value == SEM_VALUE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    __atomic_fetch_add(&semaphore->value, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&semaphore->waiters, __ATOMIC_RELAXED) > 0) {
        wake_one(&semaphore->value);
    }
    return 0;
}

int sem_getvalue(sem_t *semaphore, int *out) {
    if (!semaphore || !out) {
        errno = EINVAL;
        return -1;
    }
    int seen = __atomic_load_n(&semaphore->value, __ATOMIC_RELAXED);
    *out = seen < 0 ? 0 : seen;
    return 0;
}

/* A named semaphore outlives the process that made it, which needs a name in
   a filesystem shared memory can be opened through. This OS has no /dev/shm
   and M120's memfd is deliberately nameless. The condition for building
   these three is a shared-memory filesystem, not a place to put the code. */
sem_t *sem_open(const char *name, int flags, ...) {
    (void)name;
    (void)flags;
    errno = ENOSYS;
    return SEM_FAILED;
}

int sem_close(sem_t *semaphore) {
    (void)semaphore;
    errno = ENOSYS;
    return -1;
}

int sem_unlink(const char *name) {
    (void)name;
    errno = ENOSYS;
    return -1;
}
