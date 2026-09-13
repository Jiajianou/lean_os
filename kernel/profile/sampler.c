#include "sampler.h"

#include "library/spinlock.h"
#include "library/kernel_library.h"
#include "scheduler/scheduler.h"

static spinlock_t prof_lock;
static prof_sample_t buckets[PROF_BUCKETS];
static prof_stats_t statistics;

static uint32_t hash_key(uint64_t rip, int32_t pid) {
    uint64_t h = rip * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)(uint32_t)pid * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)((h >> 40) & (PROF_BUCKETS - 1));
}

void profile_reset(void) {
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    k_memset(buckets, 0, sizeof(buckets));
    int was_running = statistics.running;
    k_memset(&statistics, 0, sizeof(statistics));
    statistics.running = was_running;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_start(void) {
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    statistics.running = 1;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_stop(void) {
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    statistics.running = 0;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_sample(isr_regs_t *regs) {
    if (!statistics.running) {
        return;
    }

    int user = (regs->cs & 3) != 0;
    task_t *self = scheduler_current();
    int32_t pid = PROF_PID_KERNEL;
    int idle = 0;

    if (self) {
        idle = self->is_idle ? 1 : 0;
        if (user) {
            pid = (int32_t)self->id;
        }
    }
    if (!user && scheduler_task_is_idle_waiting(self)) {
        idle = 1;
    }

    uint64_t flags = spin_lock_irqsave(&prof_lock);
    statistics.samples++;
    if (idle) {
        statistics.idle++;
        spin_unlock_irqrestore(&prof_lock, flags);
        return;
    }
    if (user) {
        statistics.user++;
    } else {
        statistics.kernel++;
    }

    uint64_t rip = regs->rip;
    if (rip == 0) {
        statistics.overflow++;
        spin_unlock_irqrestore(&prof_lock, flags);
        return;
    }

    uint32_t i = hash_key(rip, pid);
    for (int probe = 0; probe < PROF_BUCKETS; probe++) {
        prof_sample_t *b = &buckets[i];
        if (b->rip == rip && b->pid == pid) {
            b->count++;
            spin_unlock_irqrestore(&prof_lock, flags);
            return;
        }
        if (b->rip == 0) {
            b->rip = rip;
            b->pid = pid;
            b->count = 1;
            statistics.distinct++;
            spin_unlock_irqrestore(&prof_lock, flags);
            return;
        }
        i = (i + 1) & (PROF_BUCKETS - 1);
    }
    statistics.overflow++;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_get_statistics(prof_stats_t *out) {
    if (!out) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    *out = statistics;
    spin_unlock_irqrestore(&prof_lock, flags);
}

int profile_snapshot(prof_sample_t *out, int max) {
    if (!out || max <= 0) {
        return 0;
    }
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    for (int i = 0; i < PROF_BUCKETS && n < max; i++) {
        if (buckets[i].rip != 0) {
            out[n++] = buckets[i];
        }
    }
    spin_unlock_irqrestore(&prof_lock, flags);
    return n;
}
