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

int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr;
    if (!m) {
        return 22;
    }
    m->locked = 0;
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
    /* Spin briefly, then yield. The spin is what makes an uncontended or
     * briefly-contended lock cost nothing but one instruction; the yield
     * is what stops a waiter from burning its whole time slice against a
     * holder that is not currently running - which, on a machine with
     * fewer cores than threads, is most of the time.
     *
     * 200 is not tuned, and saying so is more useful than pretending: it
     * is long enough to cover a critical section of a few instructions
     * (the case this library's own users have) and short enough that a
     * genuinely blocked waiter reaches the yield in microseconds. */
    for (;;) {
        for (int spin = 0; spin < 200; spin++) {
            if (atomic_xchg(&m->locked, 1) == 0) {
                return 0;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        sys_yield();
    }
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    return atomic_xchg(&m->locked, 1) == 0 ? 0 : 16 /* EBUSY */;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    /* An exchange rather than a plain store, so that unlocking is a
     * barrier too - every write the critical section made is visible
     * before the lock reads as free. */
    atomic_xchg(&m->locked, 0);
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

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (!c || !m) {
        return 22;
    }
    /* Sampled BEFORE the mutex is dropped, which is the whole
     * correctness argument: a signal that arrives in the window between
     * the unlock and the first read of the counter still moves it past
     * the value sampled here, so it cannot be missed. Sampling after the
     * unlock is the classic lost-wakeup bug. */
    unsigned observed = c->seq;
    pthread_mutex_unlock(m);
    while (c->seq == observed) {
        sys_yield();
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
        if (sys_uptime_ms() >= deadline) {
            pthread_mutex_lock(m);
            return 110; /* ETIMEDOUT */
        }
        sys_yield();
    }
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) {
        return 22;
    }
    /* Wakes every waiter, not one - see <pthread.h>. An atomic exchange
     * rather than an increment so that the write is also a barrier:
     * everything the signalling thread did before this is visible to a
     * waiter that sees the new value. */
    atomic_xchg((volatile int *)&c->seq, (int)(c->seq + 1u));
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    return pthread_cond_signal(c); /* identical here, and honestly so */
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
