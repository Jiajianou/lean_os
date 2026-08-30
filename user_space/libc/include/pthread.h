/* user_space/libc/include/pthread.h - M79
 *
 * Threads, on top of SYS_thread_create's one primitive: a second
 * schedulable context in this address space. Everything a pthread *is* -
 * a start routine with a return value, joining, a mutex - is built here,
 * in user space, out of memory the two threads now share. That is not a
 * shortcut; it is what shared memory is for, and a kernel that owned
 * `pthread_t` would be a kernel with an opinion about a C library.
 *
 * What is here is what a real program's link step actually asks for, in
 * M63's tradition of letting the program name the surface: create, join,
 * self, equal, a mutex, and a once. Deliberately absent - condition
 * variables, rwlocks, thread-local storage, cancellation, attributes
 * beyond a stack size - until something concrete fails to link without
 * them.
 *
 * Two limits worth knowing before writing against this, because neither
 * is discoverable by reading the signatures:
 *
 *  - a thread must be joined by the thread that CREATED it. The kernel's
 *    SYS_wait is what a join is built on, and it answers about a task
 *    the caller started.
 *  - threads share memory, and share the file descriptors that were open
 *    when they were created - including their file positions, because a
 *    descriptor points at a kernel-side open-file entry (M59). A
 *    descriptor opened AFTER a thread starts is not visible to it: the
 *    fd table is copied at creation the way a spawn copies it. Said here
 *    rather than discovered later.
 */
#pragma once

#include <stddef.h>

typedef int pthread_t;

typedef struct {
    size_t stack_size; /* 0 = the default, PTHREAD_STACK_DEFAULT below */
} pthread_attr_t;

/* 64 KiB. A thread's stack comes out of M78's mmap arena, so this is a
 * real allocation rather than a reservation - which is why the default
 * is modest and why pthread_attr_setstacksize exists at all. */
#define PTHREAD_STACK_DEFAULT (64u * 1024u)

/* A mutex is one word and a spin-then-yield loop. There are no futexes
 * here: a blocking mutex would need a kernel-side wait channel keyed on
 * a user address, which is a real piece of work and is not what "two
 * threads, one address space" needs to be true. What this does have is
 * the property that matters - it is a real mutual exclusion built on an
 * atomic exchange, so an increment inside it cannot interleave. */
typedef struct {
    volatile int locked;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0}

typedef struct {
    volatile int done;
    pthread_mutex_t lock;
} pthread_once_t;

#define PTHREAD_ONCE_INIT {0, PTHREAD_MUTEX_INITIALIZER}

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);

/* 0 on success, or a nonzero error the way pthread_create reports (not
 * -1 and errno - this libc has no errno, and pthread is the one API in C
 * that never used it). */
int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                    void *(*start)(void *), void *arg);

/* Waits for `thread` and, if `retval` is non-NULL, stores what its start
 * routine returned. Must be called by the thread that created it. */
int pthread_join(pthread_t thread, void **retval);

/* Ends the calling thread. Never returns. Returning from a start routine
 * is equivalent. */
void pthread_exit(void *retval) __attribute__((noreturn));

pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);

int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_once(pthread_once_t *once, void (*init)(void));
