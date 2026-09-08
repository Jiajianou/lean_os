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

/* M97: <sched.h>, because GCC's gthr-posix.h - the layer std::thread and
 * std::mutex are built on - reaches for sched_yield through <pthread.h>
 * alone. Every other pthreads implementation includes it here, and the
 * failure without it is "sched_yield undeclared" inside a GCC header
 * during a libstdc++ build, which names neither file. */
#include <sched.h>

#include <stddef.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

typedef int pthread_t;

typedef struct {
    size_t stack_size; /* 0 = the default, PTHREAD_STACK_DEFAULT below */
} pthread_attr_t;

/* 64 KiB. A thread's stack comes out of M78's mmap arena, so this is a
 * real allocation rather than a reservation - which is why the default
 * is modest and why pthread_attr_setstacksize exists at all. */
#define PTHREAD_STACK_DEFAULT (64u * 1024u)

/* ---- M96: a mutex over a futex ----------------------------------------
 *
 * M79 wrote here: "A mutex is one word and a spin-then-yield loop. There
 * are no futexes here: a blocking mutex would need a kernel-side wait
 * channel keyed on a user address, which is a real piece of work." There
 * is one now (SYS_futex), and this is that word turned into a real lock.
 *
 * Three states rather than two, and the third is the whole design:
 *
 *   0  free
 *   1  held, and nobody is waiting
 *   2  held, and somebody MAY be waiting
 *
 * The uncontended paths - lock a free mutex, unlock one nobody wants -
 * are a single atomic instruction each and never enter the kernel. Only
 * a thread that actually has to wait writes 2 and calls futex(WAIT), and
 * only an unlock that finds 2 calls futex(WAKE). That is the entire
 * reason a futex is worth having: the syscall is on the contended path,
 * where a syscall is cheap next to the wait it replaces.
 *
 * The 2 is sticky - an unlock that sees it wakes somebody even if that
 * somebody has since given up - and that is deliberate. Clearing it
 * accurately needs a count of waiters, and an unnecessary wake costs one
 * syscall while a missed one costs a hang.
 *
 * ---- M100: and an owner and a count, for the two other types ---------
 *
 * The word above is the whole of a NORMAL mutex and its fast paths are
 * unchanged. RECURSIVE and ERRORCHECK need to know who holds the lock -
 * the first so the holder can take it again, the second so the holder
 * is told rather than deadlocked - and that is the `owner` (a thread id,
 * since a thread here is a task with its own id) and, for RECURSIVE,
 * how many times. Sixteen bytes rather than four; glibc's is forty.
 *
 * Until M100 this header REFUSED the two types in settype, on M65's
 * rule, and said why: "a recursive mutex this library treated as normal
 * deadlocks the first time a program relies on the recursion, somewhere
 * far from here". sqlite is that program - its database mutex is
 * recursive by design - and it does not check settype's return value,
 * so the refusal was invisible and the deadlock arrived anyway: the
 * eighth syscall of its life was a futex wait on a mutex it held. A
 * refusal only refuses when the caller looks. */
typedef struct {
    volatile unsigned int state;
    unsigned int type;     /* PTHREAD_MUTEX_NORMAL / RECURSIVE / ERRORCHECK */
    volatile int owner;    /* the thread id holding it, for the two typed kinds; 0 when free */
    unsigned int count;    /* recursion depth, RECURSIVE only */
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0, 0, 0, 0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {0, 1, 0, 0}
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP {0, 2, 0, 0}

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
typedef struct { int type; } pthread_mutexattr_t; /* M100: the one thing there is to configure */

#define PTHREAD_ONCE_INIT {0, PTHREAD_MUTEX_INITIALIZER}

/* ---- M96: the rest of <pthread.h>, over the same futex ----------------
 *
 * M79's "deliberately absent" list said rwlocks, barriers and semaphores
 * would arrive "until something concrete fails to link without them".
 * They arrive here for a different and better reason: every one of them
 * is a wait, all four of the waits in this header were spin loops, and
 * the futex is what makes a wait cost nothing. Writing them now, over
 * one primitive, is cheaper than writing them later over four.
 *
 * A reader-writer lock is one word: the count of readers, with a
 * sentinel for "a writer holds it". A barrier is a count and a
 * generation - the generation is what stops a thread that reaches the
 * barrier twice quickly from being counted into the wrong round, which
 * is the bug every naive barrier has. A semaphore is a count.
 */
typedef struct {
    volatile unsigned int state; /* 0 free, ~0u writer, else reader count */
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER {0}

typedef struct {
    volatile unsigned int count;      /* how many have arrived this round */
    volatile unsigned int generation; /* which round - see above */
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

/* ---- M94: the attribute objects, and a signal to a thread -------------
 *
 * Not new capability - every one of these describes something this
 * library either already does or truthfully cannot. They are here
 * because a `./configure` script *probes* for them: gnulib decides
 * whether a system has a usable pthread API by compiling a program that
 * calls `pthread_kill`, `pthread_mutex_lock` and
 * `pthread_mutexattr_init`, and a system that fails that probe is told
 * to use its own replacement machinery instead - which is how GNU
 * hello's build reached a `#error` in gnulib rather than in anything
 * this project wrote.
 *
 * `pthread_kill` is the one with real behaviour behind it, and it is
 * exact rather than approximate: a thread here IS a task with its own id
 * (M79), so sending it a signal is SYS_kill with that id. On a system
 * where threads share one pid this would be the hard one; here it is the
 * easy one.
 *
 * The mutex TYPES were a truthful refusal until M100 - `settype`
 * accepted NORMAL/DEFAULT and refused the other two rather than behaving
 * like NORMAL, which would deadlock a program that relied on recursion.
 * It did anyway, because sqlite does not check what settype returns.
 * All three are real now; see the mutex's own note above.
 */
#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_DEFAULT    0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2

/* ---- M97: cancellation, which this OS does not have -------------------
 *
 * Declared because gthr-posix.h takes a weak reference to it and a weak
 * reference still needs a declaration; defined because a weak reference
 * that resolves to nothing is a call through a null pointer the first
 * time somebody takes the path that uses it.
 *
 * It refuses, and that is M65's rule rather than a shortcut: "don't
 * build a thing that pretends to enforce something". Cancellation is
 * cancellation points, cleanup handlers and a deferred/asynchronous
 * distinction - a real feature, and one nothing here has asked for. A
 * pthread_cancel that returned 0 and left the thread running would be
 * the worst possible answer: the caller would believe the thread was
 * gone and then read what it was still writing. ENOSYS says no.
 */
int pthread_cancel(pthread_t thread);

int pthread_kill(pthread_t thread, int sig);

int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *out);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *out);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);

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

#ifdef __cplusplus
}
#endif
