#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>

#include <process.h>
#include <stdint.h>

#include "syscall_wrappers.h"

typedef struct {
    void *(*start)(void *);
    void *arg;
    void *retval;
    volatile int finished;
    void *stack_base;
    size_t stack_bytes;
    void *tls;
} thread_block_t;

extern void *__lean_tls_setup(void);
extern void __lean_tls_release(void *block);
extern void __lean_run_thread_destructors(void);

/* As many threads as this machine has task slots, because that is the real
   ceiling: a thread here IS a task, and the kernel's table is MAX_TASKS.
   sysconf(_SC_THREAD_THREADS_MAX) has answered 128 since it was written, and
   this registry held 32 - so the library refused the thirty-third thread
   with EAGAIN while telling anyone who asked that it supported four times
   that many. Chromium in one process wants more than thirty-two before it
   has finished starting, and what that looks like from outside is
   "pthread_create: EAGAIN" three seconds in. */
#define MAX_THREADS 256

static struct {
    pthread_t tid;
    thread_block_t *block;
} registry[MAX_THREADS];

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

static inline int atomic_xchg(volatile int *p, int v) {
    __asm__ volatile("lock xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
}

static inline unsigned int atomic_cas(volatile unsigned int *p,
                                      unsigned int expected,
                                      unsigned int desired) {
    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "+a"(expected), "+m"(*p)
                     : "r"(desired)
                     : "memory");
    return expected;
}

static inline unsigned int atomic_xchg_u(volatile unsigned int *p,
                                         unsigned int v) {
    __asm__ volatile("lock xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
}

static void futex_wait(volatile unsigned int *p, unsigned int val,
                       unsigned int timeout_ms) {
    sys_futex(p, FUTEX_WAIT, val, timeout_ms);
}

static void futex_wake(volatile unsigned int *p, int count) {
    sys_futex(p, FUTEX_WAKE, (unsigned int)count, 0);
}

int pthread_mutex_init(pthread_mutex_t *m, const void *attribute) {
    if (!m) {
        return 22;
    }
    m->state = 0;
    m->type = attribute ? (unsigned int)((const pthread_mutexattr_t *)attribute)->type
                   : PTHREAD_MUTEX_NORMAL;
    m->owner = 0;
    m->count = 0;
    return 0;
}

static int mutex_lock_word(pthread_mutex_t *m);

static int typed_lock_prologue(pthread_mutex_t *m, int *done) {
    *done = 0;
    if (m->type == PTHREAD_MUTEX_NORMAL) {
        return 0;
    }
    if (m->owner == (int)pthread_self()) {
        *done = 1;
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        return 35;
    }
    return 0;
}

static void typed_lock_epilogue(pthread_mutex_t *m) {
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        m->owner = (int)pthread_self();
        m->count = 1;
    }
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    int done;
    int r = typed_lock_prologue(m, &done);
    if (done) {
        return r;
    }
    r = mutex_lock_word(m);
    typed_lock_epilogue(m);
    return r;
}

static int mutex_lock_word(pthread_mutex_t *m) {
    if (atomic_cas(&m->state, 0, 1) == 0) {
        return 0;
    }
    for (int spin = 0; spin < 200; spin++) {
        if (atomic_cas(&m->state, 0, 1) == 0) {
            return 0;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    while (atomic_xchg_u(&m->state, 2) != 0) {
        futex_wait(&m->state, 2, 0);
    }
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner == (int)pthread_self()) {
        m->count++;
        return 0;
    }
    if (atomic_cas(&m->state, 0, 1) != 0) {
        return 16;
    }
    typed_lock_epilogue(m);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) {
        return 22;
    }
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != (int)pthread_self()) {
            return 1;
        }
        if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count > 0) {
            return 0;
        }
        m->owner = 0;
        m->count = 0;
    }
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

int pthread_cond_init(pthread_cond_t *c, const void *attribute) {
    if (!c) {
        return 22;
    }
    c->seq = 0;
    /* The attribute is not ignored any more, and the difference is the whole
       of M169's second bug: a caller that asks for CLOCK_MONOTONIC and is
       given a variable that measures against CLOCK_REALTIME hands it
       deadlines about 1.7 billion seconds in the past. */
    c->clock = CLOCK_REALTIME;
    if (attribute) {
        const pthread_condattr_t *a = (const pthread_condattr_t *)attribute;
        if (a->clock == CLOCK_MONOTONIC || a->clock == CLOCK_REALTIME) {
            c->clock = a->clock;
        }
    }
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

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

/* How long until an absolute deadline, on the clock this variable keeps its
   deadlines on, in nanoseconds.

   Getting the clock wrong here does not fail: it returns a number, and the
   number is enormous and negative, so the wait expires immediately and the
   caller loops. Chromium's base::ConditionVariable asks for CLOCK_MONOTONIC
   and waits on it in every thread pool it has; against CLOCK_REALTIME that
   is a difference of about fifty-five years, so four of its processes spun
   on a single core for four hundred seconds and its renderer never drew
   anything. 227 million clock reads and 1,786 futex waits is what that looks
   like from underneath. */
static long long cond_remaining_ns(const pthread_cond_t *c,
                                   const struct timespec *abstime) {
    struct timespec now;
    if (clock_gettime(c->clock, &now) != 0) {
        return 0;
    }
    return ((long long)abstime->tv_sec - (long long)now.tv_sec) * 1000000000LL +
           ((long long)abstime->tv_nsec - (long long)now.tv_nsec);
}

/* M200: ETIMEDOUT only once the deadline has passed on the variable's own
   clock. This compared whole milliseconds of a millisecond clock, rounded the
   wait down, and so timed out up to a millisecond early - which a caller that
   checks the time sees as a spurious timeout and waits again for. The futex
   wait is still in milliseconds, so it is rounded UP and the loop looks at
   the real clock each time it comes back. */
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abstime) {
    if (!c || !m || !abstime) {
        return 22;
    }
    unsigned observed = c->seq;
    pthread_mutex_unlock(m);
    while (c->seq == observed) {
        long long left = cond_remaining_ns(c, abstime);
        if (left <= 0) {
            pthread_mutex_lock(m);
            return 110;
        }
        long long ms = (left + 999999LL) / 1000000LL;
        if (ms > 0x7FFFFFFFLL) {
            ms = 0x7FFFFFFFLL;
        }
        sys_futex(&c->seq, FUTEX_WAIT, observed, (unsigned int)ms);
    }
    pthread_mutex_lock(m);
    return 0;
}

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
    return cond_wake(c, MAX_THREADS);
}

int pthread_rwlock_init(pthread_rwlock_t *rw, const void *attribute) {
    (void)attribute;
    if (!rw) {
        return 22;
    }
    rw->state = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
    (void)rw;
    return 0;
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
            continue;
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
        return 16;
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
            continue;
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
        futex_wake(&rw->state, MAX_THREADS);
        return 0;
    }
    for (;;) {
        seen = rw->state;
        if (seen == 0 || seen == ~0u) {
            return 22;
        }
        if (atomic_cas(&rw->state, seen, seen - 1u) == seen) {
            if (seen == 1) {
                futex_wake(&rw->state, 1);
            }
            return 0;
        }
    }
}

int pthread_barrier_init(pthread_barrier_t *b, const void *attribute,
                         unsigned int count) {
    (void)attribute;
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
    unsigned int gen = b->generation;
    unsigned int arrived;
    for (;;) {
        arrived = b->count;
        if (atomic_cas(&b->count, arrived, arrived + 1u) == arrived) {
            break;
        }
    }
    if (arrived + 1u == b->threshold) {
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

int pthread_attr_init(pthread_attr_t *attribute) {
    if (!attribute) {
        return 22;
    }
    attribute->stack_size = 0;
    attribute->detach_state = PTHREAD_CREATE_JOINABLE;
    attribute->stack_base = 0;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attribute, void **base,
                          size_t *size) {
    if (!attribute || !base || !size) {
        return 22;
    }
    if (!attribute->stack_base) {
        return 22;
    }
    *base = attribute->stack_base;
    *size = attribute->stack_size;
    return 0;
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t *attribute) {
    if (!attribute) {
        return 22;
    }
    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (registry[i].block && registry[i].tid == thread) {
            attribute->stack_base = registry[i].block->stack_base;
            attribute->stack_size = registry[i].block->stack_bytes;
            pthread_mutex_unlock(&registry_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&registry_lock);

    /* Not in the registry, which for the thread a process starts with is not
       a failure - nothing here created it, so nothing here recorded it. The
       kernel did, and where it put the stack is part of the ABI rather than
       something to be guessed from the stack pointer: OS_MAIN_STACK_TOP and
       OS_MAIN_STACK_MAX_BYTES in system_api/include/process.h, which
       kernel/process/process.h derives its own names from.

       base is the LOWEST address and size the extent, which is what
       pthread_attr_getstack means by them and what a caller adding the two
       expects to be the top. The region is the whole one the kernel will
       grow into rather than the part that is mapped now, because a caller
       asking is asking what the thread may use.

       M155: V8 asks this on whatever thread it is on, including the first,
       and treats not knowing as fatal - Check failed: IsOnCentralStack(). */
    if (thread == pthread_self()) {
        attribute->stack_base =
            (void *)(uintptr_t)(OS_MAIN_STACK_TOP - OS_MAIN_STACK_MAX_BYTES);
        attribute->stack_size = (size_t)OS_MAIN_STACK_MAX_BYTES;
        return 0;
    }

    attribute->stack_base = 0;
    attribute->stack_size = 0;
    return ENOSYS;
}

int pthread_attr_setstacksize(pthread_attr_t *attribute, size_t size) {
    if (!attribute) {
        return 22;
    }
    attribute->stack_size = size;
    return 0;
}

pthread_t pthread_self(void) {
    return (pthread_t)sys_gettid();
}

/* A thread's name goes to the kernel rather than into a field here, because
   the whole point of naming a thread is that something else can say which
   thread it is looking at - the serial log and /bin/task_manager print
   task_t.name, and a thread on this machine is a task. Both calls act on the
   calling thread only: naming another thread is a write into a task this one
   does not own, and no caller here has ever asked for it.

   ERANGE rather than truncation is Linux's rule for the same call and it is
   the right one: a program told it named a thread "CompositorTileWorker"
   when the name it actually has is "CompositorTile" has been lied to about
   its own machine. */
int pthread_setname_np(pthread_t thread, const char *name) {
    if (!name) {
        return EINVAL;
    }
    if (thread != pthread_self()) {
        return ENOSYS;
    }
    return sys_thread_setname(name) == 0 ? 0 : ERANGE;
}

int pthread_getname_np(pthread_t thread, char *out, size_t length) {
    if (!out || length == 0) {
        return EINVAL;
    }
    if (thread != pthread_self()) {
        return ENOSYS;
    }
    return sys_thread_getname(out, (unsigned long)length) == 0 ? 0 : ERANGE;
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

static void thread_trampoline(thread_block_t *tb) {
    tb->tls = __lean_tls_setup();
    void *r = tb->start(tb->arg);
    __lean_run_thread_destructors();
    tb->retval = r;
    tb->finished = 1;
    sys_thread_exit(0);
    for (;;) {
    }
}

int pthread_create(pthread_t *out, const pthread_attr_t *attribute,
                    void *(*start)(void *), void *arg) {
    if (!out || !start) {
        return 22;
    }
    size_t want = (attribute && attribute->stack_size) ? attribute->stack_size : PTHREAD_STACK_DEFAULT;
    size_t bytes = (want + sizeof(thread_block_t) + 4095u) & ~(size_t)4095u;
    void *memory = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (memory == MAP_FAILED) {
        return 11;
    }

    unsigned char *top = (unsigned char *)memory + bytes;
    thread_block_t *tb = (thread_block_t *)(top - sizeof(thread_block_t));
    tb->start = start;
    tb->arg = arg;
    tb->retval = 0;
    tb->finished = 0;
    tb->stack_base = memory;
    tb->stack_bytes = bytes;
    tb->tls = 0;

    unsigned long stack_top = ((unsigned long)tb) & ~15UL;

    long tid = sys_thread_create((void *)thread_trampoline, tb, stack_top);
    if (tid < 0) {
        munmap(memory, bytes);
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
    if (slot < 0) {
        return 11;
    }

    *out = (pthread_t)tid;
    if (attribute && attribute->detach_state == PTHREAD_CREATE_DETACHED) {
        pthread_detach((pthread_t)tid);
    }
    return 0;
}

static int thread_is_known(pthread_t thread) {
    if (thread == pthread_self()) {
        return 1;
    }
    pthread_mutex_lock(&registry_lock);
    int found = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (registry[i].block && registry[i].tid == thread) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return found;
}

int pthread_attr_setdetachstate(pthread_attr_t *attribute, int state) {
    if (!attribute || (state != PTHREAD_CREATE_JOINABLE &&
                       state != PTHREAD_CREATE_DETACHED)) {
        return EINVAL;
    }
    attribute->detach_state = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attribute, int *out) {
    if (!attribute || !out) {
        return EINVAL;
    }
    *out = attribute->detach_state;
    return 0;
}

/* A protocol this scheduler could carry out is accepted and one it could not
   is refused. Priority inheritance needs a priority to inherit and this
   machine has two classes instead, so PTHREAD_PRIO_INHERIT returns ENOTSUP
   rather than being stored and forgotten - M65's rule about a call that
   pretends to enforce something. */
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *attribute, int protocol) {
    if (!attribute) {
        return EINVAL;
    }
    if (protocol == PTHREAD_PRIO_NONE) {
        attribute->protocol = protocol;
        return 0;
    }
    if (protocol == PTHREAD_PRIO_INHERIT || protocol == PTHREAD_PRIO_PROTECT) {
        return ENOTSUP;
    }
    return EINVAL;
}

int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *attribute, int *out) {
    if (!attribute || !out) {
        return EINVAL;
    }
    *out = attribute->protocol;
    return 0;
}

/* Both clocks are real answers now, and they have to be: the point of this
   call is that a caller waiting on a relative delay does not want its
   deadline moved by someone setting the date, and the caller that says so
   gets CLOCK_MONOTONIC while the one that says nothing gets POSIX's default.
   Refusing CLOCK_REALTIME used to be the position here, and what made it
   untenable was not the refusal but its other half: the variable measured
   against CLOCK_REALTIME whatever it had been told. */
int pthread_condattr_setclock(pthread_condattr_t *attribute, int clock) {
    if (!attribute) {
        return EINVAL;
    }
    if (clock == CLOCK_MONOTONIC || clock == CLOCK_REALTIME) {
        attribute->clock = clock;
        return 0;
    }
    return EINVAL;
}

int pthread_condattr_getclock(const pthread_condattr_t *attribute, int *out) {
    if (!attribute || !out) {
        return EINVAL;
    }
    *out = attribute->clock;
    return 0;
}

int pthread_getschedparam(pthread_t thread, int *policy,
                          struct sched_param *param) {
    if (!policy || !param) {
        return EINVAL;
    }
    if (!thread_is_known(thread)) {
        return ESRCH;
    }
    *policy = SCHED_OTHER;
    param->sched_priority = 0;
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy,
                          const struct sched_param *param) {
    if (!param) {
        return EINVAL;
    }
    if (!thread_is_known(thread)) {
        return ESRCH;
    }
    if (policy != SCHED_OTHER || param->sched_priority != 0) {
        return ENOTSUP;
    }
    return 0;
}

int pthread_detach(pthread_t thread) {
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
    return found ? 0 : 3;
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
        return 3;
    }

    sys_wait(thread);

    if (retval) {
        *retval = tb->finished ? tb->retval : 0;
    }
    void *base = tb->stack_base;
    size_t bytes = tb->stack_bytes;
    void *tls = tb->tls;

    pthread_mutex_lock(&registry_lock);
    registry[slot].block = 0;
    registry[slot].tid = 0;
    pthread_mutex_unlock(&registry_lock);

    __lean_tls_release(tls);
    munmap(base, bytes);
    return 0;
}

static struct {
    int in_use;
    void (*destructor)(void *);
} tss_keys[PTHREAD_KEYS_MAX];

static struct {
    pthread_t tid;
    const void *value[PTHREAD_KEYS_MAX];
    int used;
} tss_rows[MAX_THREADS + 1];

static pthread_mutex_t tss_lock = PTHREAD_MUTEX_INITIALIZER;

/* The destructor was accepted and discarded until M165, which is the kind of
   gap nothing finds: a thread-specific value whose destructor never runs
   looks exactly like a program that had nothing to free. Chromium's
   base::ThreadLocalStorage is built entirely out of this call and its
   OnThreadExit is where a renderer gives its per-thread state back. */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key) {
        return 22;
    }
    pthread_mutex_lock(&tss_lock);
    for (int i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (!tss_keys[i].in_use) {
            tss_keys[i].in_use = 1;
            tss_keys[i].destructor = destructor;
            for (int r = 0; r < MAX_THREADS + 1; r++) {
                tss_rows[r].value[i] = 0;
            }
            pthread_mutex_unlock(&tss_lock);
            *key = i;
            return 0;
        }
    }
    pthread_mutex_unlock(&tss_lock);
    return 11;
}

int pthread_key_delete(pthread_key_t key) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) {
        return 22;
    }
    pthread_mutex_lock(&tss_lock);
    tss_keys[key].in_use = 0;
    tss_keys[key].destructor = 0;
    pthread_mutex_unlock(&tss_lock);
    return 0;
}

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

/* POSIX's sweep, and the lock is dropped across every destructor call on
   purpose: a destructor is allowed to call pthread_setspecific - that is how
   a key is legitimately re-established, and it is why the sweep repeats -
   so holding tss_lock while calling one would deadlock on this library's own
   non-recursive mutex. The value is cleared BEFORE the call, so a destructor
   that does nothing leaves the key empty and the sweep terminates. */
void __lean_run_key_destructors(void) {
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        int any = 0;
        for (int key = 0; key < PTHREAD_KEYS_MAX; key++) {
            pthread_mutex_lock(&tss_lock);
            int r = tss_row_for_self(0);
            void (*destructor)(void *) = 0;
            void *value = 0;
            if (r >= 0 && tss_keys[key].in_use && tss_rows[r].value[key]) {
                destructor = tss_keys[key].destructor;
                value = (void *)tss_rows[r].value[key];
                tss_rows[r].value[key] = 0;
            }
            pthread_mutex_unlock(&tss_lock);
            if (destructor && value) {
                destructor(value);
                any = 1;
            }
        }
        if (!any) {
            return;
        }
    }
}

/* A row was claimed by the first pthread_setspecific a thread made and never
   given back, so a program that created and joined thirty-three threads ran
   out of rows and every setspecific after that failed with ENOMEM. The table
   is one row per LIVE thread now rather than one per thread that has ever
   existed. */
void __lean_release_thread_storage(void) {
    pthread_mutex_lock(&tss_lock);
    int r = tss_row_for_self(0);
    if (r >= 0) {
        for (int key = 0; key < PTHREAD_KEYS_MAX; key++) {
            tss_rows[r].value[key] = 0;
        }
        tss_rows[r].used = 0;
        tss_rows[r].tid = 0;
    }
    pthread_mutex_unlock(&tss_lock);
}

void pthread_exit(void *retval) {
    __lean_run_thread_destructors();
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
    }
}

int pthread_kill(pthread_t thread, int sig) {
    return sys_kill((int)thread, sig) == 0 ? 0 : 3  ;
}

int pthread_attr_destroy(pthread_attr_t *attribute) {
    (void)attribute;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attribute, size_t *out) {
    if (!attribute || !out) {
        return 22;
    }
    *out = attribute->stack_size ? attribute->stack_size : PTHREAD_STACK_DEFAULT;
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attribute) {
    if (!attribute) {
        return 22;
    }
    attribute->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attribute) {
    (void)attribute;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attribute, int type) {
    if (!attribute) {
        return 22;
    }
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
        type != PTHREAD_MUTEX_ERRORCHECK) {
        return 22;
    }
    attribute->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attribute, int *out) {
    if (!attribute || !out) {
        return 22;
    }
    *out = attribute->type;
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attribute) {
    if (!attribute) {
        return 22;
    }
    /* POSIX: the default is CLOCK_REALTIME, and a default that is not the
       standard's is a difference every caller has to know about. */
    attribute->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attribute) {
    (void)attribute;
    return 0;
}

int pthread_cancel(pthread_t thread) {
    (void)thread;
    errno = ENOSYS;
    return ENOSYS;
}

#define ATFORK_MAX 32

static struct {
    void (*prepare)(void);
    void (*parent)(void);
    void (*child)(void);
} atfork_handlers[ATFORK_MAX];

static int atfork_count;
static pthread_mutex_t atfork_lock = PTHREAD_MUTEX_INITIALIZER;

int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void)) {
    pthread_mutex_lock(&atfork_lock);
    if (atfork_count == ATFORK_MAX) {
        pthread_mutex_unlock(&atfork_lock);
        return ENOMEM;
    }
    atfork_handlers[atfork_count].prepare = prepare;
    atfork_handlers[atfork_count].parent = parent;
    atfork_handlers[atfork_count].child = child;
    atfork_count++;
    pthread_mutex_unlock(&atfork_lock);
    return 0;
}

void __lean_pthread_atfork_prepare(void) {
    int count = atfork_count;
    for (int i = count - 1; i >= 0; i--) {
        if (atfork_handlers[i].prepare) {
            atfork_handlers[i].prepare();
        }
    }
}

void __lean_pthread_atfork_parent(void) {
    int count = atfork_count;
    for (int i = 0; i < count; i++) {
        if (atfork_handlers[i].parent) {
            atfork_handlers[i].parent();
        }
    }
}

void __lean_pthread_atfork_child(void) {
    int count = atfork_count;
    for (int i = 0; i < count; i++) {
        if (atfork_handlers[i].child) {
            atfork_handlers[i].child();
        }
    }
}
