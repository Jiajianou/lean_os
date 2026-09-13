#include "timerfd.h"

#include "drivers/pit.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "scheduler/sched.h"

static spinlock_t timerfd_lock;

#define TICK_NS (1000000000ULL / PIT_HZ)

typedef struct timerfd {
    int refs;
    int clockid;
    uint64_t next_ns;
    uint64_t interval_ns;
    uint64_t expirations;
} timerfd_t;

static int live_count;

void timerfd_init(void) {
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    live_count = 0;
    spin_unlock_irqrestore(&timerfd_lock, f);
}

struct timerfd *timerfd_create(int clockid) {
    if (clockid != TIMERFD_CLOCK_REALTIME && clockid != TIMERFD_CLOCK_MONOTONIC) {
        return (timerfd_t *)0;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    if (live_count >= TIMERFD_MAX) {
        spin_unlock_irqrestore(&timerfd_lock, f);
        return (timerfd_t *)0;
    }
    timerfd_t *t = (timerfd_t *)kmalloc(sizeof(timerfd_t));
    if (!t) {
        spin_unlock_irqrestore(&timerfd_lock, f);
        return (timerfd_t *)0;
    }
    t->refs = 1;
    t->clockid = clockid;
    t->next_ns = 0;
    t->interval_ns = 0;
    t->expirations = 0;
    live_count++;
    spin_unlock_irqrestore(&timerfd_lock, f);
    return t;
}

void timerfd_ref(struct timerfd *t) {
    if (!t) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    t->refs++;
    spin_unlock_irqrestore(&timerfd_lock, f);
}

void timerfd_unref(struct timerfd *t) {
    if (!t) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    int gone = (--t->refs <= 0);
    if (gone) {
        live_count--;
    }
    spin_unlock_irqrestore(&timerfd_lock, f);
    if (gone) {
        kfree(t);
    }
}

int timerfd_clock(const struct timerfd *t) {
    return t ? t->clockid : -1;
}

static void advance(timerfd_t *t, uint64_t now_ns) {
    if (t->next_ns == 0 || now_ns < t->next_ns) {
        return;
    }
    if (t->interval_ns == 0) {
        t->expirations++;
        t->next_ns = 0;
        return;
    }
    uint64_t past = now_ns - t->next_ns;
    uint64_t extra = past / t->interval_ns;
    t->expirations += 1 + extra;
    t->next_ns += (1 + extra) * t->interval_ns;
}

static uint64_t round_to_tick(uint64_t ns) {
    if (ns == 0) {
        return 0;
    }
    uint64_t ticks = (ns + TICK_NS - 1) / TICK_NS;
    return ticks * TICK_NS;
}

int timerfd_settime(struct timerfd *t, uint64_t now_ns, int absolute,
                    uint64_t value_ns, uint64_t interval_ns,
                    uint64_t *old_value_ns, uint64_t *old_interval_ns) {
    if (!t) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    advance(t, now_ns);
    if (old_value_ns) {
        *old_value_ns = (t->next_ns > now_ns) ? t->next_ns - now_ns : 0;
    }
    if (old_interval_ns) {
        *old_interval_ns = t->interval_ns;
    }
    if (value_ns == 0) {
        t->next_ns = 0;
        t->interval_ns = 0;
        t->expirations = 0;
        spin_unlock_irqrestore(&timerfd_lock, f);
        sched_wake_all(SCHED_POLL_CHAN);
        return 0;
    }
    if (absolute) {
        t->next_ns = value_ns <= now_ns ? now_ns : round_to_tick(value_ns - now_ns) + now_ns;
    } else {
        t->next_ns = now_ns + round_to_tick(value_ns);
    }
    t->interval_ns = round_to_tick(interval_ns);
    t->expirations = 0;
    spin_unlock_irqrestore(&timerfd_lock, f);
    sched_wake_all(SCHED_POLL_CHAN);
    return 0;
}

void timerfd_gettime(const struct timerfd *t, uint64_t now_ns,
                     uint64_t *value_ns, uint64_t *interval_ns) {
    if (value_ns) {
        *value_ns = 0;
    }
    if (interval_ns) {
        *interval_ns = 0;
    }
    if (!t) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    if (value_ns && t->next_ns != 0) {
        uint64_t next = t->next_ns;
        if (next <= now_ns && t->interval_ns != 0) {
            next += ((now_ns - next) / t->interval_ns + 1) * t->interval_ns;
        }
        *value_ns = next > now_ns ? next - now_ns : 0;
    }
    if (interval_ns) {
        *interval_ns = t->interval_ns;
    }
    spin_unlock_irqrestore(&timerfd_lock, f);
}

int timerfd_read(struct timerfd *t, uint64_t now_ns, uint64_t *out) {
    if (!t || !out) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    advance(t, now_ns);
    if (t->expirations == 0) {
        spin_unlock_irqrestore(&timerfd_lock, f);
        return -1;
    }
    *out = t->expirations;
    t->expirations = 0;
    spin_unlock_irqrestore(&timerfd_lock, f);
    return 0;
}

int timerfd_readable(struct timerfd *t, uint64_t now_ns) {
    if (!t) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    advance(t, now_ns);
    int r = t->expirations > 0;
    spin_unlock_irqrestore(&timerfd_lock, f);
    return r;
}

long timerfd_next_ms(struct timerfd *t, uint64_t now_ns) {
    if (!t) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    long ms;
    if (t->expirations > 0) {
        ms = 0;
    } else if (t->next_ns == 0) {
        ms = -1;
    } else if (t->next_ns <= now_ns) {
        ms = 0;
    } else {
        uint64_t left = t->next_ns - now_ns;
        uint64_t left_ms = (left + 999999ULL) / 1000000ULL;
        ms = left_ms > 0x7FFFFFFFULL ? 0x7FFFFFFF : (long)left_ms;
    }
    spin_unlock_irqrestore(&timerfd_lock, f);
    return ms;
}

int timerfd_in_use(void) {
    uint64_t f = spin_lock_irqsave(&timerfd_lock);
    int n = live_count;
    spin_unlock_irqrestore(&timerfd_lock, f);
    return n;
}
