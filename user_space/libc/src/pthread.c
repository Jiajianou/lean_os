/* user_space/libc/src/pthread.c - M79
 *
 * See <pthread.h> for what this is and what it deliberately is not.
 *
 * The whole library rests on one observation: the two tasks share
 * memory, so everything a thread library needs to remember about a
 * thread can simply be a struct that both of them can see. There is no
 * kernel-side thread object at all - SYS_thread_create returns a task id
 * and that is the entirety of what the kernel knows.
 */
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <sys/mman.h>

#include "syscall_wrappers.h"

/* Per-thread bookkeeping, in memory both threads can read. Allocated as
 * the *top* of the thread's own stack mapping, so it is freed exactly
 * when the stack is and there is no second allocation to lose track of.
 *
 * `finished` is written by the dying thread and read by the joiner. It
 * is not a synchronisation primitive - SYS_wait is - it is what makes
 * the return value readable after the task slot is gone. */
typedef struct {
    void *(*start)(void *);
    void *arg;
    void *retval;
    volatile int finished;
    void *stack_base;   /* the mmap the whole thing lives in */
    size_t stack_bytes;
} thread_block_t;

/* MAX_THREADS join slots. A joiner needs the block after the thread is
 * gone, and the block lives in the stack the thread was running on - so
 * the stack cannot be unmapped by the thread itself. It is unmapped by
 * the join instead, which is what this table exists to make possible:
 * pthread_join needs to find the block from nothing but a tid. */
#define MAX_THREADS 32

static struct {
    pthread_t tid;
    thread_block_t *block;
} registry[MAX_THREADS];

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

/* The one atomic this library needs. `lock xchg` is a full barrier on
 * x86 and is the primitive every spinlock on this architecture is built
 * from - there is no compiler intrinsic in this freestanding build to
 * borrow, and one instruction is cheaper to read than an abstraction
 * over it. */
static inline int atomic_xchg(volatile int *p, int v) {
    __asm__ volatile("lock xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
}

/* M96: the two the futex-backed primitives need beside the exchange.
 *
 * A compare-and-swap is what makes "take it only if it is still free" a
 * single step, which is the whole of a three-state mutex and of a
 * reader-writer lock's reader count. `lock cmpxchg` is the instruction
 * and there is no intrinsic to borrow in this freestanding build - the
 * same argument atomic_xchg's own note makes. */
static inline unsigned int atomic_cas(volatile unsigned int *p,
                                      unsigned int expected,
                                      unsigned int desired) {
    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "+a"(expected), "+m"(*p)
                     : "r"(desired)
                     : "memory");
    return expected; /* what was there - equal to `expected` on success */
}

static inline unsigned int atomic_xchg_u(volatile unsigned int *p,
                                         unsigned int v) {
    __asm__ volatile("lock xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
}

/* Wait until `*p` stops being `val`, and wake whoever is waiting. Thin
 * enough to be worth naming rather than writing the syscall out four
 * times, and the names are what the rest of this file reads as. */
static void futex_wait(volatile unsigned int *p, unsigned int val,
                       unsigned int timeout_ms) {
    sys_futex(p, FUTEX_WAIT, val, timeout_ms);
}

static void futex_wake(volatile unsigned int *p, int count) {
    sys_futex(p, FUTEX_WAKE, (unsigned int)count, 0);
}

int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr;
    if (!m) {
        return 22;
    }
    m->state = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0; /* nothing was allocated, so there is nothing to release */
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    /* ---- M96: the three-state lock - see <pthread.h> for the states ---
     *
     * The uncontended acquire is the CAS below and nothing else: one
     * instruction, no syscall. Everything after it is the contended
     * path.
     *
     * A short spin before sleeping, because the common contended case on
     * this machine is a critical section of a few instructions held by a
     * thread that is currently RUNNING on another core - and a syscall
     * to sleep through that costs more than the wait. 200 is the number
     * the spin-then-yield version used and is still not tuned; what
     * changed is what happens after it, which used to be a yield that
     * came back and is now a sleep that does not. */
    if (atomic_cas(&m->state, 0, 1) == 0) {
        return 0;
    }
    for (int spin = 0; spin < 200; spin++) {
        if (atomic_cas(&m->state, 0, 1) == 0) {
            return 0;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    /* Give up spinning. From here the lock is marked 2 - "somebody may
     * be waiting" - and stays that way until it is free again, which is
     * what tells the unlocker to make a syscall. The exchange rather
     * than a CAS is deliberate: it both takes the lock if it was free
     * AND marks it contended if it was not, in one instruction, which is
     * the standard three-state futex lock and the only version of it
     * without a race between the two. */
    while (atomic_xchg_u(&m->state, 2) != 0) {
        futex_wait(&m->state, 2, 0);
    }
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    return atomic_cas(&m->state, 0, 1) == 0 ? 0 : 16 /* EBUSY */;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    /* M96: an exchange rather than a plain store, so that unlocking is a
     * barrier too - every write the critical section made is visible
     * before the lock reads as free - and so that this reads the old
     * state in the same instruction that clears it.
     *
     * The old state is what decides whether a syscall happens: 1 means
     * nobody ever had to wait, and this unlock costs one instruction; 2
     * means somebody may be asleep on it, and exactly one of them is
     * woken. See <pthread.h> for why the 2 is sticky and why an
     * unnecessary wake is the cheap side of that trade. */
    if (atomic_xchg_u(&m->state, 0) == 2) {
        futex_wake(&m->state, 1);
    }
    return 0;
}

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (!once || !init) {
        return 22;
    }
    if (once->done) {
        return 0;
    }
    pthread_mutex_lock(&once->lock);
    if (!once->done) {
        init();
        once->done = 1;
    }
    pthread_mutex_unlock(&once->lock);
    return 0;
}

/* ---- condition variables - see <pthread.h> for what this trades ------ */

int pthread_cond_init(pthread_cond_t *c, const void *attr) {
    (void)attr;
    if (!c) {
        return 22;
    }
    c->seq = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0; /* nothing was allocated */
}

/* ---- M96: the condition variables, over the futex ---------------------
 *
 * The counter and the sampling are unchanged from M79 - the correctness
 * argument is the same and is still the interesting part: `seq` is read
 * BEFORE the mutex is dropped, so a signal arriving in the window
 * between the unlock and the wait still moves the counter past the value
 * sampled, and cannot be missed. Sampling after the unlock is the
 * classic lost wakeup.
 *
 * What changed is the wait. This was `while (c->seq == observed)
 * sys_yield();` - a loop that burned a time slice per iteration for as
 * long as the wait lasted. The futex's value check IS the same test, so
 * the loop becomes a syscall that returns when the counter moves: the
 * waiting thread is TASK_BLOCKED and consumes no CPU at all, which is
 * the measurement this milestone exists for.
 *
 * The loop around the futex stays, because a futex wake is permitted to
 * be spurious and because two waiters woken by one broadcast both have
 * to re-test. POSIX allows spurious wakeups precisely so that a correct
 * program loops on its predicate; this one loops on the counter.
 */
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (!c || !m) {
        return 22;
    }
    unsigned observed = c->seq;
    pthread_mutex_unlock(m);
    while (c->seq == observed) {
        sys_futex(&c->seq, FUTEX_WAIT, observed, 0);
    }
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abstime) {
    if (!c || !m || !abstime) {
        return 22;
    }
    /* The deadline is absolute wall-clock, and this machine's wall clock
     * is SYS_time (seconds). Converted to an uptime deadline once, here,
     * so a clock that is stepped mid-wait (SYS_settime, which the SNTP
     * client does) cannot turn a one-second wait into an hour. */
    long now_s = sys_time(0);
    long remaining_ms = (abstime->tv_sec - now_s) * 1000 + abstime->tv_nsec / 1000000;
    if (remaining_ms < 0) {
        remaining_ms = 0;
    }
    long deadline = sys_uptime_ms() + remaining_ms;

    unsigned observed = c->seq;
    pthread_mutex_unlock(m);
    while (c->seq == observed) {
        long left = deadline - sys_uptime_ms();
        if (left <= 0) {
            pthread_mutex_lock(m);
            return 110; /* ETIMEDOUT */
        }
        /* The kernel's own timeout, so a wait that ends by expiring
         * costs one syscall rather than one per millisecond. The loop
         * is still here for the spurious-wake case and for a counter
         * that moved and moved back. */
        sys_futex(&c->seq, FUTEX_WAIT, observed, (unsigned int)left);
    }
    pthread_mutex_lock(m);
    return 0;
}

/* M96: signal wakes ONE and broadcast wakes all, which they could not
 * before.
 *
 * M79's note said signal "wakes every waiter, not one - which is a legal
 * implementation, not a corner cut", and it was right about the legality
 * and honest about the cost: N waiters woken for one handoff is N-1
 * context switches wasted, every time. The futex takes a count, so the
 * two calls are finally two different operations.
 *
 * The counter bump is still an atomic exchange rather than an increment,
 * so that the write is a barrier: everything the signalling thread did
 * before this is visible to a waiter that sees the new value. */
static int cond_wake(pthread_cond_t *c, int count) {
    if (!c) {
        return 22;
    }
    atomic_xchg_u(&c->seq, c->seq + 1u);
    futex_wake(&c->seq, count);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    return cond_wake(c, 1);
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    /* MAX_THREADS is this library's own ceiling on how many waiters
     * there can be, so it is "all of them" said in a number this
     * machine can mean. */
    return cond_wake(c, MAX_THREADS);
}

/* ---- M96: reader-writer locks and barriers ---------------------------
 *
 * Both are new, and both are the futex used the way <pthread.h> says: a
 * word that changes, a wait on the old value, a wake on the change.
 *
 * The rwlock's word is the reader count, with ~0u meaning a writer holds
 * it. That encoding is what makes both acquires a single compare-and-
 * swap: a reader adds one to anything that is not ~0u, a writer swaps
 * ~0u in for 0 and nothing else.
 *
 * **Writers can starve here, and that is stated rather than fixed.** A
 * stream of readers that never lets the count reach zero keeps a writer
 * out forever. Fixing it needs a "writer waiting" flag that readers
 * respect, which is a second word and a second set of wakes; nothing on
 * this machine has a reader stream dense enough for it to matter, and
 * M69's rule is that the measurement is what would change this.
 */
int pthread_rwlock_init(pthread_rwlock_t *rw, const void *attr) {
    (void)attr;
    if (!rw) {
        return 22;
    }
    rw->state = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
    (void)rw;
    return 0; /* nothing was allocated - the same answer the mutex gives */
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
    if (!rw) {
        return 22;
    }
    for (;;) {
        unsigned int seen = rw->state;
        if (seen != ~0u) {
            if (atomic_cas(&rw->state, seen, seen + 1u) == seen) {
                return 0;
            }
            continue; /* somebody else changed it first - look again */
        }
        futex_wait(&rw->state, ~0u, 0);
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw) {
    if (!rw) {
        return 22;
    }
    unsigned int seen = rw->state;
    if (seen == ~0u) {
        return 16; /* EBUSY */
    }
    return atomic_cas(&rw->state, seen, seen + 1u) == seen ? 0 : 16;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
    if (!rw) {
        return 22;
    }
    for (;;) {
        if (atomic_cas(&rw->state, 0, ~0u) == 0) {
            return 0;
        }
        unsigned int seen = rw->state;
        if (seen == 0) {
            continue; /* it went free between the CAS and the read */
        }
        futex_wait(&rw->state, seen, 0);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rw) {
    if (!rw) {
        return 22;
    }
    return atomic_cas(&rw->state, 0, ~0u) == 0 ? 0 : 16;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
    if (!rw) {
        return 22;
    }
    unsigned int seen = rw->state;
    if (seen == ~0u) {
        atomic_xchg_u(&rw->state, 0);
        /* A writer let go: every reader waiting may proceed, and one
         * writer may. Waking everybody is right here rather than
         * wasteful - the readers genuinely can all run. */
        futex_wake(&rw->state, MAX_THREADS);
        return 0;
    }
    for (;;) {
        seen = rw->state;
        if (seen == 0 || seen == ~0u) {
            return 22; /* unlocking a lock this thread does not hold */
        }
        if (atomic_cas(&rw->state, seen, seen - 1u) == seen) {
            if (seen == 1) {
                /* The last reader. Only now can a writer take it, so
                 * this is the only reader-unlock that has to wake
                 * anybody. */
                futex_wake(&rw->state, 1);
            }
            return 0;
        }
    }
}

int pthread_barrier_init(pthread_barrier_t *b, const void *attr,
                         unsigned int count) {
    (void)attr;
    if (!b || count == 0) {
        return 22;
    }
    b->count = 0;
    b->generation = 0;
    b->threshold = count;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) {
    (void)b;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b) {
    if (!b || b->threshold == 0) {
        return 22;
    }
    /* The generation is read BEFORE this thread is counted in, and is
     * what the wait is keyed on. Without it, a thread that reaches the
     * barrier, is released, loops, and reaches it again before a slower
     * sibling has woken would be counted into the next round while the
     * previous one is still finishing - which is the bug every barrier
     * written without a generation has, and it presents as an occasional
     * hang rather than as anything a test would name. */
    unsigned int gen = b->generation;
    unsigned int arrived;
    for (;;) {
        arrived = b->count;
        if (atomic_cas(&b->count, arrived, arrived + 1u) == arrived) {
            break;
        }
    }
    if (arrived + 1u == b->threshold) {
        /* The last to arrive resets the round and releases everybody.
         * The count goes first: a thread released here may loop back
         * and start the next round before this function returns. */
        atomic_xchg_u(&b->count, 0);
        atomic_xchg_u(&b->generation, gen + 1u);
        futex_wake(&b->generation, MAX_THREADS);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (b->generation == gen) {
        futex_wait(&b->generation, gen, 0);
    }
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) {
        return 22;
    }
    attr->stack_size = 0;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
    if (!attr) {
        return 22;
    }
    attr->stack_size = size;
    return 0;
}

pthread_t pthread_self(void) {
    return (pthread_t)sys_gettid();
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

/* The entry point the kernel actually drops the new thread into. Its job
 * is the one thing SYS_thread_create's contract says it must do: never
 * return. A start routine that returns falls into pthread_exit here,
 * which is exactly what POSIX says returning means. */
static void thread_trampoline(thread_block_t *tb) {
    void *r = tb->start(tb->arg);
    tb->retval = r;
    tb->finished = 1;
    sys_thread_exit(0);
    for (;;) {
        /* Unreachable - SYS_thread_exit does not return. */
    }
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                    void *(*start)(void *), void *arg) {
    if (!out || !start) {
        return 22;
    }
    size_t want = (attr && attr->stack_size) ? attr->stack_size : PTHREAD_STACK_DEFAULT;
    /* Room for the block at the top, and page-rounded because that is
     * what the arena hands out anyway. */
    size_t bytes = (want + sizeof(thread_block_t) + 4095u) & ~(size_t)4095u;
    void *mem = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) {
        return 11; /* EAGAIN - "no resources", which is what this is */
    }

    /* The block sits at the very top of the mapping and the stack grows
     * down from just below it. One allocation, one lifetime, and the
     * block survives the thread because the mapping is freed by the
     * *joiner* rather than by the thread that ran on it - a thread
     * cannot unmap the stack it is standing on. */
    unsigned char *top = (unsigned char *)mem + bytes;
    thread_block_t *tb = (thread_block_t *)(top - sizeof(thread_block_t));
    tb->start = start;
    tb->arg = arg;
    tb->retval = 0;
    tb->finished = 0;
    tb->stack_base = mem;
    tb->stack_bytes = bytes;

    unsigned long stack_top = ((unsigned long)tb) & ~15UL;

    long tid = sys_thread_create((void *)thread_trampoline, tb, stack_top);
    if (tid < 0) {
        munmap(mem, bytes);
        return 11;
    }

    pthread_mutex_lock(&registry_lock);
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (registry[i].block == 0) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        registry[slot].tid = (pthread_t)tid;
        registry[slot].block = tb;
    }
    pthread_mutex_unlock(&registry_lock);
    /* A full registry is not a failed create - the thread is running and
     * will do its work. What is lost is the ability to join it and
     * reclaim its stack, which is reported so a caller that cares can
     * act on it rather than finding out by leaking. */
    if (slot < 0) {
        return 11;
    }

    *out = (pthread_t)tid;
    return 0;
}

int pthread_detach(pthread_t thread) {
    /* Releases the join slot. The stack stays mapped - a detached thread
     * is one nobody will collect, and there is no hook here that runs
     * after its last instruction to unmap the stack it is standing on.
     * That is a leak of one mapping per detached thread and it is said
     * out loud rather than hidden: a program that detaches in a loop
     * will run out of arena, and the fix is a kernel-side "free this
     * mapping after the task is gone", which is a real piece of work
     * nothing has asked for yet. */
    pthread_mutex_lock(&registry_lock);
    int found = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (registry[i].block && registry[i].tid == thread) {
            registry[i].block = 0;
            registry[i].tid = 0;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return found ? 0 : 3; /* ESRCH */
}

int pthread_join(pthread_t thread, void **retval) {
    thread_block_t *tb = 0;
    int slot = -1;
    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (registry[i].block && registry[i].tid == thread) {
            tb = registry[i].block;
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    if (!tb) {
        return 3; /* ESRCH - not a thread this process created, or already joined */
    }

    /* The kernel's wait is what actually blocks; `finished` is only how
     * the return value gets across. A thread that was killed rather than
     * returning leaves finished == 0 and a NULL retval, which is the
     * honest answer for a thread that never produced one. */
    sys_wait(thread);

    if (retval) {
        *retval = tb->finished ? tb->retval : 0;
    }
    void *base = tb->stack_base;
    size_t bytes = tb->stack_bytes;

    pthread_mutex_lock(&registry_lock);
    registry[slot].block = 0;
    registry[slot].tid = 0;
    pthread_mutex_unlock(&registry_lock);

    /* Freed here and not by the thread itself, for the reason above: a
     * thread cannot unmap the stack it is standing on. This is the whole
     * reason pthread_join exists in every implementation of it. */
    munmap(base, bytes);
    return 0;
}

/* ---- thread-specific storage - see <pthread.h> ------------------------
 *
 * One slot per (key, thread), found by a linear scan of the same
 * registry pthread_join uses plus a row for the main thread. Linear
 * because PTHREAD_KEYS_MAX is 32 and MAX_THREADS is 32: the scan is a
 * handful of comparisons and it needs no allocation, which matters for a
 * function a runtime may call on a path where allocation is exactly what
 * it is trying to avoid.
 */
static struct {
    int in_use;
} tss_keys[PTHREAD_KEYS_MAX];

static struct {
    pthread_t tid;
    const void *value[PTHREAD_KEYS_MAX];
    int used;
} tss_rows[MAX_THREADS + 1]; /* +1 for the thread that was never created by pthread_create */

static pthread_mutex_t tss_lock = PTHREAD_MUTEX_INITIALIZER;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor; /* accepted and never called - see <pthread.h> */
    if (!key) {
        return 22;
    }
    pthread_mutex_lock(&tss_lock);
    for (int i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (!tss_keys[i].in_use) {
            tss_keys[i].in_use = 1;
            /* Every thread's value for a freshly created key must be
             * NULL, including a thread that used this slot under a
             * previous key. */
            for (int r = 0; r < MAX_THREADS + 1; r++) {
                tss_rows[r].value[i] = 0;
            }
            pthread_mutex_unlock(&tss_lock);
            *key = i;
            return 0;
        }
    }
    pthread_mutex_unlock(&tss_lock);
    return 11; /* EAGAIN - out of keys, which is what POSIX says this is */
}

int pthread_key_delete(pthread_key_t key) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) {
        return 22;
    }
    pthread_mutex_lock(&tss_lock);
    tss_keys[key].in_use = 0;
    pthread_mutex_unlock(&tss_lock);
    return 0;
}

/* The caller's row, created on first use. NULL only if the table is
 * full, which is a process with more threads than MAX_THREADS. */
static int tss_row_for_self(int create) {
    pthread_t self = pthread_self();
    for (int r = 0; r < MAX_THREADS + 1; r++) {
        if (tss_rows[r].used && tss_rows[r].tid == self) {
            return r;
        }
    }
    if (!create) {
        return -1;
    }
    for (int r = 0; r < MAX_THREADS + 1; r++) {
        if (!tss_rows[r].used) {
            tss_rows[r].used = 1;
            tss_rows[r].tid = self;
            for (int i = 0; i < PTHREAD_KEYS_MAX; i++) {
                tss_rows[r].value[i] = 0;
            }
            return r;
        }
    }
    return -1;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) {
        return 0;
    }
    pthread_mutex_lock(&tss_lock);
    int r = tss_row_for_self(0);
    const void *v = (r >= 0) ? tss_rows[r].value[key] : 0;
    pthread_mutex_unlock(&tss_lock);
    return (void *)v;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) {
        return 22;
    }
    pthread_mutex_lock(&tss_lock);
    int r = tss_row_for_self(1);
    if (r < 0) {
        pthread_mutex_unlock(&tss_lock);
        return 11;
    }
    tss_rows[r].value[key] = value;
    pthread_mutex_unlock(&tss_lock);
    return 0;
}

void pthread_exit(void *retval) {
    /* Reached directly rather than through the trampoline. The block is
     * at a fixed offset from the top of the mapping this thread's stack
     * lives in, but a thread that calls pthread_exit from arbitrary
     * depth cannot compute that from RSP without knowing the size - so
     * the registry is consulted instead, which is what it is for. */
    pthread_t self = pthread_self();
    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (registry[i].block && registry[i].tid == self) {
            registry[i].block->retval = retval;
            registry[i].block->finished = 1;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    sys_thread_exit(0);
    for (;;) {
        /* Unreachable. */
    }
}
