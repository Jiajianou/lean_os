#include "eventfd.h"

#include "drivers/kernel_log.h"

#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

static spinlock_t eventfd_lock;

typedef struct eventfd {
    uint64_t count;
    int refs;
    int semaphore;
} eventfd_t;

static int live_count;
static int refusals;

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
        refusals++;
        if (refusals == 1 || refusals % 64 == 0) {
            kernel_log_puts("[eventfd] the machine is at its ceiling of ");
            kernel_log_put_dec(EVENTFD_MAX);
            kernel_log_puts(" eventfds (");
            kernel_log_put_dec((uint32_t)refusals);
            kernel_log_puts(" refusal(s) since boot)\n");
        }
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

void eventfd_reference(struct eventfd *e) {
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
    scheduler_wake_object(e);
    return 0;
}

int eventfd_write(struct eventfd *e, uint64_t v) {
    if (!e) {
        return -1;
    }
    if (v == 0) {
        return -2;
    }
    if (v == 0xFFFFFFFFFFFFFFFFULL) {
        return -2;
    }
    uint64_t f = spin_lock_irqsave(&eventfd_lock);
    if (e->count > EVENTFD_MAX_COUNT - v) {
        spin_unlock_irqrestore(&eventfd_lock, f);
        return -1;
    }
    e->count += v;
    spin_unlock_irqrestore(&eventfd_lock, f);
    scheduler_wake_object(e);
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
