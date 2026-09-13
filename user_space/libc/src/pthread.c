#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>

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

#define MAX_THREADS 32

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
    (void)attribute;
    if (!c) {
        return 22;
    }
    c->seq = 0;
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

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abstime) {
    if (!c || !m || !abstime) {
        return 22;
    }
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
            return 110;
        }
        sys_futex(&c->seq, FUTEX_WAIT, observed, (unsigned int)left);
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
    return 0;
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

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

static void thread_trampoline(thread_block_t *tb) {
    tb->tls = __lean_tls_setup();
    void *r = tb->start(tb->arg);
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

    free(tls);
    munmap(base, bytes);
    return 0;
}

static struct {
    int in_use;
} tss_keys[PTHREAD_KEYS_MAX];

static struct {
    pthread_t tid;
    const void *value[PTHREAD_KEYS_MAX];
    int used;
} tss_rows[MAX_THREADS + 1];

static pthread_mutex_t tss_lock = PTHREAD_MUTEX_INITIALIZER;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor;
    if (!key) {
        return 22;
    }
    pthread_mutex_lock(&tss_lock);
    for (int i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (!tss_keys[i].in_use) {
            tss_keys[i].in_use = 1;
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

void pthread_exit(void *retval) {
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
    attribute->unused = 0;
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
