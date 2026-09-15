#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timespec;

/* An unnamed semaphore is a counter and a futex on it, which is what every
   other blocking primitive in this libc already is. Named semaphores are
   not here: they need a name in a filesystem that outlives the process, and
   this OS has no /dev/shm. sem_open returns ENOSYS until it does. */
typedef struct {
    volatile int value;
    volatile int waiters;
} sem_t;

#define SEM_FAILED ((sem_t *)0)
#define SEM_VALUE_MAX 2147483647

int sem_init(sem_t *semaphore, int shared, unsigned int value);
int sem_destroy(sem_t *semaphore);
int sem_wait(sem_t *semaphore);
int sem_trywait(sem_t *semaphore);
int sem_timedwait(sem_t *semaphore, const struct timespec *deadline);
int sem_post(sem_t *semaphore);
int sem_getvalue(sem_t *semaphore, int *out);

sem_t *sem_open(const char *name, int flags, ...);
int sem_close(sem_t *semaphore);
int sem_unlink(const char *name);

#ifdef __cplusplus
}
#endif
