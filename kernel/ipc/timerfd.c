/* kernel/ipc/timerfd.c - M119. See timerfd.h, including why every call
 * takes the time as an argument instead of reading a clock. */
#include "timerfd.h"

#include "drivers/pit.h" /* PIT_HZ: the granularity this file rounds to, and the one number it takes from the machine */
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "sched/sched.h"

static spinlock_t timerfd_lock;

/* One tick, in nanoseconds. The whole of what this file knows about the
 * hardware: a deadline is rounded UP to a multiple of it, because a timer
 * that is readable before the time it was asked for is a pump that spins.
 */
#define TICK_NS (1000000000ULL / PIT_HZ)

typedef struct timerfd {
    int refs;
    int clockid;
    uint64_t next_ns;     /* when it fires next; 0 when disarmed */
    uint64_t interval_ns; /* 0 for one-shot */
    uint64_t expirations; /* fired and not yet read */
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

/* Brings a timer forward to `now_ns`, counting every firing it passed.
 * Called with the lock held by everything below, which is why it is the
 * only function here that does not take it: a version that did would be a
 * second chance to get the ordering wrong.
 *
 * The loop is bounded by arithmetic rather than by iteration: a 1 ms
 * periodic timer that nobody read for an hour has expired 3.6 million
 * times, and counting that by stepping would be a three-million-iteration
 * loop inside a spinlock with interrupts off. Division instead. */
static void advance(timerfd_t *t, uint64_t now_ns) {
    if (t->next_ns == 0 || now_ns < t->next_ns) {
        return;
    }
    if (t->interval_ns == 0) {
        t->expirations++;
        t->next_ns = 0; /* one-shot: disarmed by firing */
        return;
    }
    uint64_t past = now_ns - t->next_ns;
    uint64_t extra = past / t->interval_ns;
    t->expirations += 1 + extra;
    t->next_ns += (1 + extra) * t->interval_ns;
}

/* A deadline, rounded up to the clock this machine actually has. Zero
 * stays zero: that is "disarm", not "as soon as possible". */
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
        /* Disarm. An interval with no value is this case too, and POSIX
         * says so - `{0, 1000000}` is not a periodic timer, it is off. */
        t->next_ns = 0;
        t->interval_ns = 0;
        t->expirations = 0;
        spin_unlock_irqrestore(&timerfd_lock, f);
        sched_wake_all(SCHED_POLL_CHAN);
        return 0;
    }
    if (absolute) {
        /* A deadline already in the past fires at once, which is what
         * Linux does and is not the same as refusing it: a program that
         * computes "now + 5 ms" and is preempted for 10 has asked for
         * something reasonable. */
        t->next_ns = value_ns <= now_ns ? now_ns : round_to_tick(value_ns - now_ns) + now_ns;
    } else {
        t->next_ns = now_ns + round_to_tick(value_ns);
    }
    t->interval_ns = round_to_tick(interval_ns);
    t->expirations = 0; /* a re-armed timer does not report the old one's firings */
    spin_unlock_irqrestore(&timerfd_lock, f);
    /* A waiter parked on this descriptor has a deadline computed from the
     * old setting, which may be "never". It has to be told. */
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
    /* Deliberately const: this reports, it does not advance. A disarmed
     * one-shot reads as 0 whether it was never armed or has already
     * fired, which is what Linux reports and is the one place the two
     * states are indistinguishable - the expiration count is where a
     * caller learns the difference. */
    if (value_ns && t->next_ns != 0) {
        /* For a periodic timer whose deadline is already behind us and
         * which nobody has read, the answer is the time to the NEXT
         * firing rather than zero - zero is what a disarmed timer reports,
         * and the two must not look alike to a caller that is deciding
         * whether to re-arm. Computed rather than advanced, because this
         * call reports and does not change anything. */
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
        ms = 0; /* already readable: do not park at all */
    } else if (t->next_ns == 0) {
        ms = -1; /* disarmed: nothing to wait for here */
    } else if (t->next_ns <= now_ns) {
        ms = 0;
    } else {
        uint64_t left = t->next_ns - now_ns;
        uint64_t left_ms = (left + 999999ULL) / 1000000ULL;
        /* Clamped rather than truncated: a timer 50 days out parks for
         * about that long and is re-asked, which is correct and is not
         * the same as parking for 49 days by overflow. */
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
