#include "sampler.h"

#include "lib/spinlock.h"
#include "lib/libk.h"
#include "sched/sched.h"

/* One table, one lock, and no per-CPU sharding - which is a decision
 * against the obvious optimisation and worth the sentence.
 *
 * Sampling happens at PIT_HZ per CPU: 800 acquisitions a second on an
 * eight-core machine, each holding the lock for a hash and a short
 * probe. (M106: "per CPU" became true rather than aspirational - until
 * then profile_sample was only called from pit.c, which is the BSP's
 * interrupt alone. See smp.c's lapic_vector_handler.) Sharding would remove contention that has never been measured
 * and would multiply this table's memory by MAX_CPUS, for a structure
 * whose whole job is to be read as one merged histogram anyway. If a
 * profile of the profiler ever shows this lock, that is a measurement
 * and this comment is what it contradicts. */
static spinlock_t prof_lock;
static prof_sample_t buckets[PROF_BUCKETS];
static prof_stats_t stats;

/* Fibonacci hashing, mixed down to the table's index bits. RIPs are
 * dense and highly correlated in their low bits (instructions in one
 * function are adjacent), so the multiply is what keeps a hot function's
 * addresses from probing into one cluster. */
static uint32_t hash_key(uint64_t rip, int32_t pid) {
    uint64_t h = rip * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)(uint32_t)pid * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)((h >> 40) & (PROF_BUCKETS - 1));
}

void profile_reset(void) {
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    k_memset(buckets, 0, sizeof(buckets));
    int was_running = stats.running;
    k_memset(&stats, 0, sizeof(stats));
    stats.running = was_running;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_start(void) {
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    stats.running = 1;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_stop(void) {
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    stats.running = 0;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_sample(isr_regs_t *regs) {
    /* Read without the lock on purpose. This runs on every timer tick
     * whether or not anybody is profiling, so the stopped case has to
     * cost a load and a branch rather than a lock acquisition. The race
     * it admits is one sample at the instant of a start or stop, which
     * is not a thing any consumer of this data can distinguish from the
     * tick landing 10 ms later. */
    if (!stats.running) {
        return;
    }

    int user = (regs->cs & 3) != 0;
    task_t *self = sched_current();
    int32_t pid = PROF_PID_KERNEL;
    int idle = 0;

    if (self) {
        idle = self->is_idle ? 1 : 0;
        if (user) {
            pid = (int32_t)self->id;
        }
    }
    /* Two different kinds of "not doing work", and the second one was
     * missed on the first attempt.
     *
     * `is_idle` marks the idle *task* - a CPU with nothing runnable at
     * all. sched_cpu_is_idle asks a narrower and more common question: is
     * this CPU inside a sched_idle_enter/exit bracket, which is what an
     * ordinary task halting in pit_sleep_ms is doing. That case is in
     * ring 0 at a real address, so without this check it lands in the
     * histogram as a hot kernel function - and the very first profile
     * this machine produced reported `pit_sleep_ms+0x50` as 50% of
     * kernel time. It is the `hlt`.
     *
     * Per-task rather than per-CPU, which is the second attempt: the
     * per-CPU counter M68 keeps stays raised when the timer schedules
     * away from a halted task, so the next task to run looked idle while
     * it was working. That classified an entire kernel busy loop as idle
     * and this milestone's own self-test caught it. */
    if (!user && sched_task_is_idle_waiting(self)) {
        idle = 1;
    }

    uint64_t flags = spin_lock_irqsave(&prof_lock);
    stats.samples++;
    if (idle) {
        /* A halted CPU is not where the time went, and counting it as a
         * hot address in `sched_idle_loop` is how a profile learns to
         * lie about an idle machine. Counted separately so the report
         * can say "40% idle" instead of hiding it in the histogram. */
        stats.idle++;
        spin_unlock_irqrestore(&prof_lock, flags);
        return;
    }
    if (user) {
        stats.user++;
    } else {
        stats.kernel++;
    }

    uint64_t rip = regs->rip;
    if (rip == 0) {
        /* 0 is the empty-slot marker, so an actual RIP of 0 cannot be
         * stored. It also means the machine is executing a null pointer,
         * which the fault handler is about to have opinions about. */
        stats.overflow++;
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
            stats.distinct++;
            spin_unlock_irqrestore(&prof_lock, flags);
            return;
        }
        i = (i + 1) & (PROF_BUCKETS - 1);
    }
    /* Full. Reported rather than dropped quietly: a profile taken
     * against a full table is a profile of the first 2048 addresses the
     * workload happened to touch, which is a different thing from a
     * profile and the reader has to be told. */
    stats.overflow++;
    spin_unlock_irqrestore(&prof_lock, flags);
}

void profile_get_stats(prof_stats_t *out) {
    if (!out) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&prof_lock);
    *out = stats;
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
