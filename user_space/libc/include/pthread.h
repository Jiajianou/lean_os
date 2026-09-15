#pragma once

#include <sched.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int pthread_t;

typedef struct {
    size_t stack_size;
    void *stack_base;
} pthread_attr_t;

#define PTHREAD_STACK_DEFAULT (64u * 1024u)

typedef struct {
    volatile unsigned int state;
    unsigned int type;
    volatile int owner;
    unsigned int count;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0, 0, 0, 0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {0, 1, 0, 0}
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP {0, 2, 0, 0}

typedef struct {
    volatile int done;
    pthread_mutex_t lock;
} pthread_once_t;

typedef struct {
    volatile unsigned seq;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0}

typedef struct { int unused; } pthread_condattr_t;
typedef struct { int type; } pthread_mutexattr_t;

#define PTHREAD_ONCE_INIT {0, PTHREAD_MUTEX_INITIALIZER}

typedef struct {
    volatile unsigned int state;
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER {0}

typedef struct {
    volatile unsigned int count;
    volatile unsigned int generation;
    unsigned int threshold;
} pthread_barrier_t;

typedef struct { int unused; } pthread_rwlockattr_t;
typedef struct { int unused; } pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

int pthread_rwlock_init(pthread_rwlock_t *rw, const void *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rw);
int pthread_rwlock_rdlock(pthread_rwlock_t *rw);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw);
int pthread_rwlock_wrlock(pthread_rwlock_t *rw);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rw);
int pthread_rwlock_unlock(pthread_rwlock_t *rw);

int pthread_barrier_init(pthread_barrier_t *b, const void *attr,
                         unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_DEFAULT    0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2

int pthread_cancel(pthread_t thread);

int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void));

void __lean_pthread_atfork_prepare(void);
void __lean_pthread_atfork_parent(void);
void __lean_pthread_atfork_child(void);

int pthread_kill(pthread_t thread, int sig);

int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *out);
int pthread_attr_getstack(const pthread_attr_t *attr, void **base, size_t *size);
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *out);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                    void *(*start)(void *), void *arg);

int pthread_join(pthread_t thread, void **retval);

int pthread_detach(pthread_t thread);

void pthread_exit(void *retval) __attribute__((noreturn));

pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);

int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_once(pthread_once_t *once, void (*init)(void));

struct timespec;
int pthread_cond_init(pthread_cond_t *c, const void *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

typedef int pthread_key_t;

#define PTHREAD_KEYS_MAX 32

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

#ifdef __cplusplus
}
#endif
