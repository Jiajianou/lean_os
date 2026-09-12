/* kernel/ipc/eventfd.c - M119. See eventfd.h. */
#include "eventfd.h"

#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "sched/sched.h"

/* One lock for every eventfd and the live count, for the reason
 * kernel/ipc/pipe.c gives: the critical sections are an addition long,
 * and a lock inside the object would have to be initialised by each path
 * that makes one. Interrupts off while held - a dying task unrefs its
 * descriptors, and that path is reachable from a timer tick delivering
 * SIGKILL. */
static spinlock_t eventfd_lock;

typedef struct eventfd {
    uint64_t count;
    int refs;
    int semaphore;
} eventfd_t;

static int live_count;

void eventfd_init(void) {
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    live_count = 0;
    spin_unlock_irqrestore(&eventfd_lock, f);
}

struct eventfd *eventfd_create(uint64_t initval, int semaphore) {
    if (initval > EVENTFD_MAX_COUNT) {
        return (eventfd_t *)0;
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    if (live_count >= EVENTFD_MAX) {
        spin_unlock_irqrestore(&eventfd_lock, f);
        return (eventfd_t *)0;
    }
    eventfd_t *e = (eventfd_t *)kmalloc(sizeof(eventfd_t));
    if (!e) {
        spin_unlock_irqrestore(&eventfd_lock, f);
        return (eventfd_t *)0;
    }
    e->count = initval;
    e->refs = 1;
    e->semaphore = semaphore ? 1 : 0;
    live_count++;
    spin_unlock_irqrestore(&eventfd_lock, f);
    return e;
}

void eventfd_ref(struct eventfd *e) {
    if (!e) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    e->refs++;
    spin_unlock_irqrestore(&eventfd_lock, f);
}

void eventfd_unref(struct eventfd *e) {
    if (!e) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    int gone = (--e->refs <= 0);
    if (gone) {
        live_count--;
    }
    spin_unlock_irqrestore(&eventfd_lock, f);
    if (gone) {
        /* Nothing is queued inside an eventfd - no descriptors, no bytes -
         * so unlike unixsock_unref this needs no second phase outside the
         * lock. A counter going away is a kfree. */
        kfree(e);
    }
}

int eventfd_read(struct eventfd *e, uint64_t *out) {
    if (!e || !out) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    if (e->count == 0) {
        spin_unlock_irqrestore(&eventfd_lock, f);
        return -1;
    }
    if (e->semaphore) {
        *out = 1;
        e->count--;
    } else {
        *out = e->count;
        e->count = 0;
    }
    spin_unlock_irqrestore(&eventfd_lock, f);
    /* A reader made space, and a writer that hit saturation is parked on
     * it. The wake is unconditional rather than "only if it was
     * saturated": a spurious one costs one trip round a loop that
     * re-asks, and the condition it would have to test is exactly the
     * thing the other task is about to re-read anyway. */
    sched_wake_all(SCHED_POLL_CHAN);
    return 0;
}

int eventfd_write(struct eventfd *e, uint64_t v) {
    if (!e) {
        return -1;
    }
    if (v == 0) {
        return -2; /* Linux: a no-op, not an error, and not a wait either */
    }
    if (v == 0xFFFFFFFFFFFFFFFFULL) {
        return -2; /* Linux refuses this value outright - it is the one a read can never report */
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    if (e->count > EVENTFD_MAX_COUNT - v) {
        spin_unlock_irqrestore(&eventfd_lock, f);
        return -1; /* would block: a reader has to take some first */
    }
    e->count += v;
    spin_unlock_irqrestore(&eventfd_lock, f);
    sched_wake_all(SCHED_POLL_CHAN);
    return 0;
}

int eventfd_readable(const struct eventfd *e) {
    if (!e) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    int r = e->count > 0;
    spin_unlock_irqrestore(&eventfd_lock, f);
    return r;
}

int eventfd_writable(const struct eventfd *e) {
    if (!e) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    int w = e->count < EVENTFD_MAX_COUNT;
    spin_unlock_irqrestore(&eventfd_lock, f);
    return w;
}

int eventfd_in_use(void) {
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    int n = live_count;
    spin_unlock_irqrestore(&eventfd_lock, f);
    return n;
}
