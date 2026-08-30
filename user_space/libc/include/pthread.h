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

/* ---- condition variables ----------------------------------------------
 *
 * M79's own "deliberately not" list named these, "beyond whatever the
 * next real program's link errors ask for". CPython is that program:
 * `Include/internal/pycore_condvar.h` will not compile without
 * `pthread_cond_t`, because the GIL itself is a lock and a condition
 * variable.
 *
 * One counter, and no wait queue. A waiter samples the counter, drops
 * the mutex, and spins on SYS_yield until the counter moves; a signal
 * bumps it. That means `pthread_cond_signal` wakes *every* waiter rather
 * than one - which is a legal implementation, not a corner cut: POSIX
 * permits spurious wakeups precisely so that a correct program re-tests
 * its predicate in a loop, and one that does not is broken against every
 * real implementation too. What it costs is efficiency under many
 * waiters, and what it buys is not needing a kernel-side wait channel
 * keyed on a user address - which is a futex, and a real piece of work.
 *
 * A waiter also burns CPU while it waits, which a futex would not. That
 * is the honest headline limitation and the reason this is written down
 * here: on a machine where the scheduler yields on demand (M69's quantum
 * is one tick) a spinning waiter costs a slice, not a core.
 */
typedef struct {
    volatile unsigned seq;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0}

/* Attribute types, present so that a program declaring one compiles.
 * There is nothing to configure on either - a mutex here is one word and
 * a condition variable is one counter - so the init/destroy pair is
 * accepted and does nothing, which is the truthful implementation rather
 * than a stub. */
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int unused; } pthread_mutexattr_t;

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

/* Says "nobody will join this thread". Here it releases the join slot
 * and the stack is then freed when the thread's own trampoline
 * finishes - which is the one case where a thread CAN free its own
 * stack, because after pthread_detach nothing else will. Returns 0, or
 * ESRCH for a thread this process did not create. */
int pthread_detach(pthread_t thread);

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

struct timespec;
int pthread_cond_init(pthread_cond_t *c, const void *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
/* `abstime` is an absolute wall-clock deadline, as POSIX specifies.
 * Returns 0, or ETIMEDOUT (110) if the deadline passed first. */
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

/* ---- thread-specific storage ------------------------------------------
 *
 * Added because CPython's own headers stop the build without it -
 * `Include/cpython/pythread.h` refuses to compile against a system with
 * no `NATIVE_TSS_KEY_T`, which is M63's "let the program name the
 * surface" rule producing its first demand at this layer.
 *
 * Implemented as a table indexed by (key, thread) rather than as real
 * thread-local storage, and the difference is worth knowing: there is no
 * TLS in this toolchain's freestanding mode and no per-thread base
 * register set up by the kernel, so `__thread` does not work here. A
 * table does, at the cost of a fixed ceiling on keys and threads and a
 * lookup instead of a register-relative load. That is the right trade
 * for a feature whose users call it a handful of times per thread.
 */
typedef int pthread_key_t;

#define PTHREAD_KEYS_MAX 32

/* `destructor` is accepted and NOT called, and that is the one place
 * this differs from POSIX in a way a program could notice. Running a
 * destructor means running user code on a thread that is already
 * leaving, which needs a hook in the exit path this library does not
 * have; a destructor that is silently never called is a leak, and one
 * that ran at the wrong moment would be a crash. Said here rather than
 * discovered. */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
