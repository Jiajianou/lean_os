#include "sched.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/io.h" /* irq_save_disable/irq_restore */
#include "arch/x86_64/smp.h"
#include "drivers/klog.h"
#include "drivers/pit.h"
#include "fs/openfile.h"
#include "net/socket.h"
#include "ipc/pipe.h"
#include "ipc/shm.h" /* shm_free_by_owner - see task_exit_with_code */
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "panic.h"
#include "proc/proc.h" /* M54 - process_destroy_address_space, called from task_exit_with_code */
#include "signal.h" /* system_api/include/signal.h - SIGKILL/SIGTERM */

#define TASK_STACK_SIZE (8 * 1024)
/* M69: 5 -> 1. The quantum was the latency floor and nothing else.
 *
 * Round-robin gives every runnable task the CPU for a whole slice, so the
 * worst case for "the compositor wants to run" was one slice per task
 * ahead of it. At 50 ms that made a cursor move cost 98 ms with a single
 * CPU-bound task on the machine - measured, see the [m69] self-test.
 *
 * The trade is real and it is the right way round for a desktop: more
 * context switches for lower latency. One PIT tick is the finest slice
 * this clock can express, a switch costs a register save and a possible
 * CR3 reload, and 100 of those a second is nothing next to what it buys.
 * A batch machine would want the opposite and this is not one. */
#define SCHED_QUANTUM_TICKS 2 /* two 10 ms PIT ticks - see the measurements above */

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);

static task_t tasks[MAX_TASKS];
static int task_count;

/* SMP: every CPU shares the one `tasks` table above (there's no per-CPU
 * run queue or task affinity - any online CPU can pick up any READY
 * task), but "which task is *this* CPU currently running" and "how far
 * into this CPU's own time slice are we" are inherently per-CPU the
 * moment a second core can be genuinely executing a different task at the
 * same instant. */
static task_t *current_task[MAX_CPUS];
static uint32_t ticks_in_slice[MAX_CPUS];
static uint32_t aging_ticks; /* M69: BSP-only counter driving the anti-starvation pass */

/* M69: "somebody more urgent than the running task became runnable".
 *
 * Priorities alone did not move input-to-photon at all under load, and
 * this is why: waking a task makes it READY, but nothing reschedules
 * until the running task's quantum expires. So the compositor could be
 * woken by a mouse interrupt, outrank all four CPU-bound tasks, and still
 * wait out somebody else's slice before running - which is precisely the
 * latency the classes were supposed to remove.
 *
 * Set by a wake that promotes somebody above the current task, honoured
 * by the very next timer tick. A flag rather than a schedule() call
 * inside sched_wake_all, because that function is called from interrupt
 * handlers and from inside other subsystems' locks, and rescheduling
 * from there would be re-entering the scheduler at an arbitrary point.
 * The cost of the indirection is at most one tick. */
static volatile int need_resched[MAX_CPUS];
static uint64_t loaded_pml4_phys[MAX_CPUS]; /* mirrors whatever schedule() last loaded into this CPU's own CR3, so same-address-space switches (the common case: plain kernel tasks) skip a needless TLB-flushing reload */

/* Guards `tasks`/`task_count` and the pick-next/state-transition half of
 * schedule() - NOT the context_switch() call itself, which can't run
 * while holding a lock a *different* CPU might need in order to make
 * progress (this CPU doesn't "return" from context_switch until this
 * exact task is resumed, possibly by another core, possibly much later).
 *
 * Ownership protocol (the same technique teaching kernels like xv6 use for
 * exactly this problem): the OUTGOING task acquires the lock in
 * schedule(), marks itself READY and the next task RUNNING, then calls
 * context_switch still holding it. The lock is released by whichever
 * task/CPU resumes *this* task next - either right after schedule()'s own
 * context_switch call returns (the normal "preempted before, running
 * again now" case, at the bottom of schedule() below), or at the top of
 * task_entry_trampoline for a task that has never run before. Both are
 * genuine resume points symmetric with the acquire above, so exactly one
 * release always pairs with exactly one acquire, regardless of which CPU
 * ends up doing which half.
 *
 * Every acquire of this lock (schedule(), task_spawn_common, sched_init_ap)
 * is wrapped in irq_save_disable/irq_restore (arch/x86_64/io.h), not just
 * spin_lock/spin_unlock - found necessary by testing, not anticipated in
 * advance: without it, IPI_SCHEDULE_VECTOR (the scheduler-tick broadcast,
 * smp.c) landing on a CPU that already holds sched_lock reenters
 * schedule() from inside its own interrupt handler and deadlocks on a
 * lock this exact CPU already holds - a plain `cli` isn't optional
 * hardening here the way it might look, it's what keeps this CPU's own
 * interrupt handlers out of a critical section it's already inside. The
 * saved flags are an ordinary local variable, so for schedule() itself
 * they naturally travel with whichever task's stack they were pushed on
 * and get restored correctly whenever - and on whichever CPU - that exact
 * task resumes, the same way the lock ownership itself does. */
static spinlock_t sched_lock;

/* See sched.h. Bumped by every wake, sampled by every lockless waiter. */
static uint64_t event_seq;

/* M68: how many tasks are currently TASK_BLOCKED.
 *
 * sched_wake_all is called on every pipe write, every keystroke and every
 * arriving packet, and a 128-entry scan under a lock with interrupts off
 * is not free on any of those paths - the compositor writes an event per
 * mouse move. This makes the overwhelmingly common case ("nobody is
 * waiting on anything") a single compare. Measured the hard way: without
 * it, the boot's animation self-test started missing its 16 ms frame
 * budget, which is the harness noticing a cost that no assertion in this
 * file would have. */
static int blocked_count;

static task_t *pick_next(task_t *from);
static void wake_expired(uint64_t now_ms);
static void unblock_self(task_t *self);

/* M68: the well-known wait channels. Their *addresses* are the identity;
 * the values are never read. `const int` rather than a macro so that two
 * of them can never accidentally be the same address. */
const int sched_poll_channel = 0;
const int sched_keyboard_channel = 0;
/* Nothing ever wakes this one - a timed sleep ends only when its deadline
 * passes, and giving it its own address keeps a stray sched_wake_all from
 * cutting somebody's sleep short. */
const int sched_sleep_channel = 0;

/* M68: per-CPU tick accounting. `idle_ticks` counts the ticks on which
 * this CPU had nothing runnable but its own idle identity - which is the
 * measurement this milestone exists to move, and which was exactly zero
 * on every core before it. Plain uint64_t rather than atomics: each is
 * written only by its own CPU's tick handler, and a reader that catches a
 * torn 64-bit read gets a number that is off by one tick out of hundreds. */
static uint64_t idle_ticks[MAX_CPUS];
static uint64_t total_ticks[MAX_CPUS];

void sched_debug_dump(const char *label) {
    klog_puts("[sched-dump] ");
    klog_puts(label);
    klog_putc('\n');
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_FREE) {
            continue;
        }
        klog_puts("  slot=");
        klog_put_dec((uint32_t)i);
        klog_puts(" pid=");
        klog_put_dec((uint32_t)t->id);
        klog_puts(" name=");
        klog_puts(t->name);
        klog_puts(" state=");
        klog_put_dec((uint32_t)t->state);
        klog_puts(" chan=0x");
        klog_put_hex32((uint32_t)(uint64_t)t->wait_chan);
        klog_puts(" deadline=");
        klog_put_dec((uint32_t)t->wake_deadline_ms);
        klog_putc('\n');
    }
}

/* Per-CPU, because "which CPU is halted" is the question - and only the
 * CPU itself ever writes its own entry, from a context that is by
 * definition not concurrent with itself. */
static int idle_depth[MAX_CPUS];

void sched_sleep_until(uint64_t deadline_ms) {
    int cpu = smp_current_cpu();
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    task_t *self = current_task[cpu];
    self->wait_chan = SCHED_SLEEP_CHAN;
    self->wake_deadline_ms = deadline_ms;
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&sched_lock);
    irq_restore(flags);

    schedule();
    unblock_self(self);
}

void sched_idle_enter(void) {
    uint64_t flags = irq_save_disable();
    idle_depth[smp_current_cpu()]++;
    irq_restore(flags);
}

void sched_idle_exit(void) {
    uint64_t flags = irq_save_disable();
    int cpu = smp_current_cpu();
    if (idle_depth[cpu] > 0) {
        idle_depth[cpu]--;
    }
    irq_restore(flags);
}

/* M68: one per CPU, and the reason they exist is a bug rather than tidiness.
 *
 * Before M68 there was always something runnable - every "blocking" call
 * was a spin, so the run queue was never empty and pick_next could always
 * return somebody. Once tasks genuinely leave the run queue, "nothing to
 * run" becomes an ordinary state, and it arrives at the two worst
 * possible moments: a task that has just blocked, and a task that has
 * just terminated. Neither can be handed the CPU back. The second one
 * panicked - "task_exit: terminated task resumed" - the first time every
 * other task on the machine happened to be asleep at once.
 *
 * `sti` explicitly, rather than trusting the flags a brand-new task
 * inherits from context_switch: a `hlt` with interrupts off is the
 * unwakeable halt M64 documented, and this is the one task in the system
 * whose entire body is a `hlt`. */
static void idle_task_body(void *arg) {
    (void)arg;
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void sched_spawn_idle_tasks(int cpus) {
    for (int i = 0; i < cpus; i++) {
        task_t *t = task_spawn("idle", idle_task_body, (void *)0);
        if (!t) {
            panic("sched: could not spawn an idle task");
        }
        t->is_idle = 1;
        t->parent_id = -1; /* nobody's child - power_orderly_stop exempts these */
    }
}

void sched_mark_self_idle(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    current_task[smp_current_cpu()]->is_idle = 1;
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

uint64_t sched_idle_ticks(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? idle_ticks[cpu] : 0;
}

uint64_t sched_total_ticks(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? total_ticks[cpu] : 0;
}

/* M45: bounded copy into a task_t's own fixed name buffer. NULL and an
 * over-long name are both ordinary inputs here, not errors - see
 * TASK_NAME_MAX's own comment in sched.h. */
static void set_task_name(task_t *t, const char *name) {
    int i = 0;
    if (name) {
        for (; name[i] && i < TASK_NAME_MAX - 1; i++) {
            t->name[i] = name[i];
        }
    }
    t->name[i] = '\0';
}

static void task_entry_trampoline(void) {
    spin_unlock(&sched_lock); /* pairs with the acquire in schedule() that first picked this task to run */
    task_t *t = current_task[smp_current_cpu()];
    t->entry(t->arg);
    task_exit();
}

/* Terminates a task in response to a pending fatal signal (SIGKILL or
 * SIGTERM - the only two this project recognizes, both with the same
 * "just terminate" default action). Exit code follows the standard
 * shell convention (128 + signal number) so SYS_wait's caller can tell a
 * signal death from a normal exit. */
static void deliver_pending_signal_and_exit(task_t *t) __attribute__((noreturn));
static void deliver_pending_signal_and_exit(task_t *t) {
    int sig = t->pending_signal;
    t->pending_signal = 0;
    task_exit_with_code(128 + sig);
}

/* Runs inside IRQ0's handler on every real PIT tick (BSP only - the 8259
 * only ever delivers to the BSP, see pic.c) and, via
 * IPI_SCHEDULE_VECTOR (kernel/arch/x86_64/smp.c's lapic_vector_handler),
 * once per broadcast for every other online CPU too - AP preemption is
 * "piggyback off the one real hardware timer", not a separate per-core
 * APIC timer, a deliberate simplification (see the SMP progress log
 * entry). Checks the *current* task's pending signal first - this is
 * what catches a task that never makes a syscall (a tight compute loop,
 * say) within one time slice, since syscall_handler's own check
 * (kernel/arch/x86_64/syscall.c) would otherwise never run for it. Only
 * actually reschedules once every SCHED_QUANTUM_TICKS, so a task gets a
 * real time slice rather than being preempted on every single 10 ms
 * tick. */
void scheduler_tick_cpu(int cpu) {
    task_t *t = current_task[cpu];

    /* M68: the measurement. "This CPU had nothing to do on this tick"
     * means the task it is running is one of the idle identities - task 0
     * after handoff, or an AP's cpu-idle - and nothing else was READY.
     * Both halves matter: an idle identity that is running *because* the
     * run queue is genuinely empty is a sleeping machine, and one running
     * while real work waits its turn is a scheduling bug.
     *
     * Counted here rather than in the idle loop itself because the tick
     * is the only clock this kernel has that fires whether or not
     * anything is running. */
    total_ticks[cpu]++;
    if (t->is_idle || idle_depth[cpu] > 0) {
        /* "...and there is no real work waiting" - which needs the lock,
         * because it walks the table. An idle task running while real
         * work is queued behind it is not idle time, and counting it as
         * such would make this the flattering kind of measurement.
         *
         * Asked as "is any non-idle task READY" rather than by reusing
         * pick_next, because pick_next answers a different question:
         * given several idle tasks it will happily return a *different*
         * one, so `pick_next(t) == t` is false on an idle machine and the
         * measurement read zero for exactly that reason. Two functions
         * that both mean "is there anything to do" and disagree is one
         * function too many. */
        uint64_t f = irq_save_disable();
        spin_lock(&sched_lock);
        int nothing_else = 1;
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].state == TASK_READY && !tasks[i].is_idle) {
                nothing_else = 0;
                break;
            }
        }
        spin_unlock(&sched_lock);
        irq_restore(f);
        if (nothing_else) {
            idle_ticks[cpu]++;
        }
    }

    /* Deadlines, before the signal check below: a task whose sleep just
     * expired should be runnable on this tick rather than the next one. */
    wake_expired(pit_get_ticks() * (1000 / PIT_HZ));

    /* M76: any nonzero pending_signal at all, not just the two that used
     * to be the only ones SYS_kill would accept. sched_raise_signal is
     * now the only thing that sets this field, and it only ever sets it
     * for a signal whose *disposition on this task* is death - a caught
     * one goes in sig_pending instead. So "there is a fatal signal
     * pending" and "pending_signal != 0" became the same statement. */
    if (t->pending_signal != 0) {
        deliver_pending_signal_and_exit(t);
    }
    /* M69: aging. Nothing stays batch for more than a second without
     * another chance to prove itself; a genuinely CPU-bound task
     * re-demotes within SCHED_BATCH_THRESHOLD slices, so the promise
     * costs about 2% of a core. BSP only - one cadence for one property. */
    if (cpu == 0 && ++aging_ticks >= SCHED_AGING_TICKS) {
        aging_ticks = 0;
        uint64_t af = irq_save_disable();
        spin_lock(&sched_lock);
        for (int i = 0; i < task_count; i++) {
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].last_block_tick = pit_get_ticks();
            need_resched[smp_current_cpu()] = 1;
        }
        spin_unlock(&sched_lock);
        irq_restore(af);
    }

    /* M69: an interactive task became runnable while somebody else held
     * the CPU. Cut the slice short rather than making it wait - that
     * wait was the whole of the load penalty the classes did not fix. */
    int preempt = need_resched[cpu] && t->prio == PRIO_BATCH;
    need_resched[cpu] = 0;
    if (!preempt && ++ticks_in_slice[cpu] < SCHED_QUANTUM_TICKS) {
        return;
    }
    ticks_in_slice[cpu] = 0;

    /* Preempted rather than woken: this task wanted its whole slice. */
    if (t->full_slices < 255) {
        t->full_slices++;
    }
    /* Demote only a task that has burned its slices AND has not blocked
     * recently. The second half is what keeps the compositor interactive
     * through an animation, during which it spins on purpose - see
     * task_t.last_block_tick. */
    if (t->full_slices >= SCHED_BATCH_THRESHOLD &&
        pit_get_ticks() - t->last_block_tick > SCHED_AGING_TICKS) {
        t->prio = PRIO_BATCH;
    }
    schedule();
}

static void scheduler_tick(void) {
    scheduler_tick_cpu(0); /* the BSP - the only CPU the real PIT interrupt ever reaches */
    smp_broadcast_schedule_tick(); /* everyone else, via IPI - no-op single-core */
}

void sched_init(void) {
    tasks[0].state = TASK_RUNNING;
    tasks[0].generation = 0;
    tasks[0].id = PID_MAKE(0, 0); /* == 0, which is what keeps kernel_main's own identity unchanged across M54 */
    tasks[0].stack_base = NULL; /* this is kernel_main's own stack, not one we allocated or will ever free */
    tasks[0].kernel_stack_top = 0; /* never consulted: RSP0 only matters for a ring3->ring0 transition, and task 0 never runs in ring 3 */
    tasks[0].pml4_phys = vmm_kernel_pml4_phys();
    /* M63: task 0's own FPU image. It is in BSS, so an uninitialised one
     * would be all zeros - and an all-zero MXCSR unmasks every SIMD
     * exception, which the first task switched *to* task 0 would then be
     * running under. */
    fpu_state_init(tasks[0].fpu_state);
    tasks[0].fds[0].type = FD_STDIN;
    tasks[0].fds[1].type = FD_STDOUT;
    tasks[0].parent_id = -1;
    tasks[0].pgid = 0;
    /* M65: the root of the whole model. Every capability any process on
     * this machine will ever hold is a subset of this one, arrived at by
     * a chain of spawns that can only narrow. */
    tasks[0].caps = CAP_ALL;
    /* M75: the root of the other inheritance chain. Everything spawned on
     * this machine descends from task 0, so its directory is what "/" as
     * a default actually means - and BSS would otherwise leave it the
     * empty string, which is not a path. */
    tasks[0].tgid = tasks[0].id; /* M79: a process is its own thread group, and task 0 is a process */
    tasks[0].cwd[0] = '/';
    tasks[0].cwd[1] = '\0';
    tasks[0].env_block = NULL;
    tasks[0].env_len = 0;
    tasks[0].env_count = 0;
    set_task_name(&tasks[0], "kernel");
    task_count = 1;
    current_task[0] = &tasks[0];
    loaded_pml4_phys[0] = tasks[0].pml4_phys;

    pit_set_tick_hook(scheduler_tick);
}

void sched_init_ap(int cpu_id) {
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    int slot = task_count;
    task_t *t = &tasks[slot];
    t->state = TASK_RUNNING;
    t->generation = 0;
    t->id = PID_MAKE(slot, 0);
    t->stack_base = NULL; /* this is ap_main's own boot stack (smp.c's start_ap kmalloc'd it), not one this table owns or will ever free */
    t->kernel_stack_top = 0; /* like task 0, never consulted - this idle identity never enters ring 3 */
    t->pml4_phys = vmm_kernel_pml4_phys();
    fpu_state_init(t->fpu_state); /* M63: this AP's idle identity, same reason as task 0's */
    t->fds[0].type = FD_STDIN;
    t->fds[1].type = FD_STDOUT;
    t->parent_id = -1;
    t->pgid = 0;
    t->pending_signal = 0;
    t->reaped = 0;
    t->caps = CAP_ALL; /* M65: a kernel idle identity, which never enters ring 3 and never makes a syscall */
    t->is_idle = 1;    /* M68 */
    t->tgid = t->id;   /* M79 */
    t->cwd[0] = '/';   /* M75: never used - an idle identity makes no syscalls - but not left as the empty string */
    t->cwd[1] = '\0';
    t->env_block = NULL;
    t->env_len = 0;
    t->env_count = 0;
    set_task_name(t, "cpu-idle");
    current_task[cpu_id] = t;
    loaded_pml4_phys[cpu_id] = t->pml4_phys;
    task_count++;
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

/* M79: `thread_of` is NULL for everything that is not a thread, which is
 * every caller that existed before this milestone. It is a parameter
 * rather than something the one thread-creating caller sets afterwards
 * for exactly the reason the M40 note further down gives about
 * heap_brk: this task is schedulable the instant sched_lock drops, and a
 * thread that ran for one instruction believing it was a process would
 * consult its own (zero) heap break the first time it called malloc -
 * a mapping at virtual address 0 and an immediate panic. Publishing a
 * fully-initialised task is the only version of this that is correct. */
static task_t *task_spawn_common(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                                  uint64_t heap_start, uint64_t shm_base, task_t *thread_of) {
    /* kmalloc takes its own lock (heap.c) - done before sched_lock so the
     * two are never nested in the reverse order anywhere in this kernel
     * (see heap.c's own note on lock ordering). */
    uint8_t *stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack_base) {
        panic("task_spawn: out of heap memory for a task stack");
    }

    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    /* M54: the first free slot, which may be one a reaped task gave back
     * rather than a brand-new one past the high-water mark. Scanning is
     * fine at MAX_TASKS = 128 and costs nothing next to the address space
     * this task is about to be given; a free list would be one more thing
     * to keep in step with `state` for no measurable gain. */
    int slot = -1;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (task_count >= MAX_TASKS) {
            spin_unlock(&sched_lock);
            irq_restore(flags);
            kfree(stack_base);
            return NULL;
        }
        slot = task_count++;
    }
    task_t *t = &tasks[slot];
    task_t *caller = current_task[smp_current_cpu()];
    /* The generation is bumped on *release* (sched_reap_slot), not here,
     * so a slot that has never been used keeps generation 0 and its first
     * occupant's pid is just its slot number - which is what makes the
     * ids in a boot log still readable. */
    t->id = PID_MAKE(slot, t->generation);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->pml4_phys = pml4_phys;
    t->stack_base = stack_base;
    t->kernel_stack_top = (uint64_t)(stack_base + TASK_STACK_SIZE);

    /* Inherits the spawning task's whole fd table (so a pipe fd set up
     * before SYS_spawn carries over to the child, the same way a real
     * fork() would inherit descriptors) and process group; parent_id
     * records who to attribute this task to for SYS_wait(-1). */
    for (int i = 0; i < MAX_FDS; i++) {
        t->fds[i] = caller->fds[i];
        fd_retain(&t->fds[i]); /* M59: the child holds these too - see fd_retain */
    }
    t->parent_id = caller->id;
    t->pgid = caller->pgid;
    t->pending_signal = 0;
    t->reaped = 0;
    /* M75: the working directory is inherited exactly the way the fd
     * table above is, and for the same reason - a child is launched *in*
     * a place, and a launcher that had to pass one as an argument would
     * be a launcher every program had to agree with. The environment is
     * NOT copied here: it is set by process_spawnve after this returns,
     * because that is the layer that knows whether the caller asked for
     * inheritance or supplied one, and a kmalloc under sched_lock would
     * invert this kernel's one lock ordering rule (heap before sched -
     * see the note at the top of this function). */
    for (int i = 0; i < PATH_MAX_LEN; i++) {
        t->cwd[i] = caller->cwd[i];
        if (!caller->cwd[i]) {
            break;
        }
    }
    if (!t->cwd[0]) {
        t->cwd[0] = '/';
        t->cwd[1] = '\0';
    }
    t->env_block = NULL;
    t->env_len = 0;
    t->env_count = 0;
    /* M76: signal dispositions are reset, not inherited - a handler
     * address belongs to the image that installed it, and this task is
     * about to be given a different one. Done HERE, inside the same
     * critical section that publishes the task as TASK_READY, for the
     * same reason heap_brk is (see the M40 note below): the task is
     * schedulable the instant the lock drops, and a slot whose previous
     * occupant terminated without being reaped still holds that
     * occupant's handler table. Doing it only in sched_reap_slot covers
     * the recycled case and misses exactly that one. */
    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = SIG_DFL_ADDR;
    }
    t->sig_restorer = 0;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    /* M78: an empty arena. Not inherited for the same reason the heap
     * cursor above is not: this is a fresh address space, so every
     * address the parent had mapped means nothing here. */
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i].base = 0;
        t->mmaps[i].pages = 0;
    }
    /* M79: a process is its own thread group; a thread belongs to the
     * group of whoever created it. */
    t->tgid = thread_of ? thread_of->tgid : t->id;
    t->is_thread = thread_of ? 1 : 0;
    t->exiting = 0;
    /* M65: inherited by default, and the spawn path narrows it
     * immediately afterward - see process_spawn_capped. Inheriting first
     * and narrowing second, rather than the other way round, means a
     * caller that forgets gets the *old* behaviour rather than a process
     * with no capabilities that fails in a confusing way. */
    t->caps = caller->caps;
    /* M63: a clean FPU, not an inherited one. Deliberately *not* copied
     * from the caller the way the fd table above is: descriptors are
     * something a child is meant to inherit and floating-point registers
     * are not - they are scratch, and starting from somebody else's
     * would be starting from noise. */
    fpu_state_init(t->fpu_state);

    /* M40: set *here*, inside the same critical section that publishes
     * this task as TASK_READY, not by the caller afterwards. proc.c used
     * to fill these three in on the task_spawn_in return path, with a
     * comment arguing the gap was too short to matter - it wasn't. The
     * task is schedulable the instant the lock drops, and once it runs it
     * gets a whole quantum, which is far more than enough to reach crt0,
     * malloc and SYS_sbrk with heap_brk still zero: a mapping at virtual
     * address 0 and an immediate panic. (Found the moment MAX_FDS grew,
     * which lengthened the fd-copy loop just above and shifted the timing
     * enough to make the race fire on every boot instead of never - see
     * milestones.md's M40 section.) The window can't be closed by
     * disabling interrupts around the caller's assignments either: on SMP
     * another CPU's scheduler can claim a READY task regardless of this
     * one's interrupt flag. Publishing a fully-initialized task is the
     * only version of this that's actually correct. */
    t->heap_brk = heap_start;
    t->heap_mapped_end = heap_start;
    t->shm_next_vaddr = shm_base;
    /* M45: inside the same critical section, and for the same reason -
     * see this function's note just above. A SYS_taskinfo call running on
     * another CPU can read this table the instant the lock drops. */
    set_task_name(t, name);

    /* Fabricate a stack that looks exactly like a task that's already
     * mid-context_switch: context_switch's `ret` will pop
     * task_entry_trampoline as if it were resuming a call, the six pops
     * before it load harmless zeros into the callee-saved registers
     * (their real values don't matter - the task has never run, nothing
     * depends on them yet), and `popfq` needs a real RFLAGS value with IF
     * set - see context_switch.asm's header comment for why a fresh task
     * would otherwise start with interrupts silently disabled. 0x202 =
     * bit 1 (always set) | bit 9 (IF). */
    uint64_t *sp = (uint64_t *)t->kernel_stack_top;
    *(--sp) = (uint64_t)task_entry_trampoline; /* popped by `ret` */
    *(--sp) = 0x202; /* rflags, popped by `popfq` */
    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */
    t->rsp = (uint64_t)sp;

    spin_unlock(&sched_lock);
    irq_restore(flags);
    return t;
}

task_t *task_spawn(const char *name, void (*entry)(void *arg), void *arg) {
    /* A plain kernel thread never reaches SYS_sbrk/SYS_shm_map (both are
     * ring-3-only paths), so its user-VM cursors stay zero - the same
     * "meaningless, left zeroed" contract task_t's own field comments
     * already describe. */
    return task_spawn_common(name, vmm_kernel_pml4_phys(), entry, arg, 0, 0, (task_t *)0);
}

task_t *task_spawn_in(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                       uint64_t heap_start, uint64_t shm_base) {
    return task_spawn_common(name, pml4_phys, entry, arg, heap_start, shm_base, (task_t *)0);
}

/* Round-robin: scan forward from `from`, wrapping, for the next READY
 * task. TASK_RUNNING (i.e. `from` itself, if nothing else is ready) and
 * TASK_TERMINATED slots are skipped. Caller must hold sched_lock - two
 * CPUs scanning/claiming concurrently without it could both pick the same
 * READY task. */
static task_t *pick_next(task_t *from) {
    /* M54: from->id is a pid now, not an index - PID_SLOT is where in the
     * table it actually lives. A free slot is skipped for the same reason
     * a terminated one is: it holds no runnable task.
     *
     * M68: TASK_BLOCKED is skipped by the same test, for free - it is not
     * TASK_READY. That is the whole of "a blocked task does not get the
     * CPU", and it is why the state had to be a state rather than a flag. */
    int start = PID_SLOT(from->id);
    task_t *idle = (task_t *)0;
    task_t *batch = (task_t *)0;
    for (int offset = 1; offset <= task_count; offset++) {
        int i = (start + offset) % task_count;
        if (tasks[i].state != TASK_READY) {
            continue;
        }
        /* M69: batch is remembered as a fallback, not returned.
         * Round-robin order is preserved within each class. */
        if (tasks[i].prio == PRIO_BATCH && !tasks[i].is_idle) {
            if (!batch) {
                batch = &tasks[i];
            }
            continue;
        }
        /* M68: an idle task is a last resort, never a peer. It is always
         * READY by construction, so letting it take its turn in the
         * rotation would hand it a full quantum's share of the CPU on a
         * busy machine - it exists to be somewhere to go, not to run. */
        if (tasks[i].is_idle) {
            if (!idle) {
                idle = &tasks[i];
            }
            continue;
        }
        return &tasks[i];
    }
    /* Nothing else has real work. Carry on with the caller if it still
     * can - that is the original behaviour and the common case. */
    if (batch) {
        return batch; /* nothing interactive wants the CPU - compute away */
    }
    if (!from->is_idle && (from->state == TASK_RUNNING || from->state == TASK_READY)) {
        return from;
    }
    /* The caller cannot continue (it blocked, or it just terminated) and
     * there is no other work. This is what the idle tasks are for, and
     * why they are spawned at all: without one, schedule() would have to
     * hand the CPU back to a task that has no business running, which is
     * how M68 first produced "task_exit: terminated task resumed" on a
     * machine where every other task was asleep. */
    return idle ? idle : from;
}

/* M68: wake anything whose deadline has passed. Called from the tick, so
 * it runs with interrupts already off and with sched_lock NOT held - it
 * takes it itself. A deadline of 0 means "no deadline" and is the common
 * case, so the test is one compare for almost every slot. */
static void wake_expired(uint64_t now_ms) {
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    if (blocked_count == 0) {
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return;
    }
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wake_deadline_ms != 0 &&
            now_ms >= tasks[i].wake_deadline_ms) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            tasks[i].prio = PRIO_INTERACTIVE; /* M69 - see sched_wake_all */
            tasks[i].full_slices = 0;
            tasks[i].last_block_tick = pit_get_ticks();
            need_resched[smp_current_cpu()] = 1;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
        }
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
}



uint64_t sched_event_seq(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    uint64_t v = event_seq;
    spin_unlock(&sched_lock);
    irq_restore(flags);
    return v;
}

void sched_wake_all(const void *chan) {
    if (!chan) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    /* Bumped even when nothing is parked on this channel: the whole point
     * is to tell a waiter that has not managed to fall asleep yet that it
     * missed something, and such a waiter is by definition not on any
     * channel to be counted. */
    event_seq++;
    if (blocked_count == 0) {
        /* The counter bump above still has to happen - a waiter that has
         * not fallen asleep yet is not counted here and is exactly who it
         * is for. */
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return;
    }
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            /* M69: THE signal - see prio_class_t. A spinning task never
             * reaches this line; a blocking one always does. */
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
        }
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

/* See sched.h for the contract. The ordering here is the entire point and
 * is worth reading in order:
 *
 *   1. mark this task BLOCKED, while the caller's condition lock is still
 *      held - so no waker can have run since the caller tested it;
 *   2. drop the condition lock - now a waker may run, and may set this
 *      task back to READY before step 3, which is harmless;
 *   3. schedule() - which will not pick this task again while it is
 *      BLOCKED, and will pick it normally if step 2's waker un-blocked it;
 *   4. retake the condition lock, so the caller's re-test loop sees a
 *      consistent world.
 *
 * A fatal signal is checked before parking. Without that, a task that
 * blocks forever on a channel nobody wakes is a task SIGKILL cannot
 * reach - which is precisely the "unkillable process" M42 fixed for the
 * old spin-based version of this, and it would have come straight back. */
/* M68: the exit half of a block, and it has to be symmetric with the
 * entry half or blocked_count drifts.
 *
 * schedule() can return with this task STILL marked BLOCKED - that is the
 * "nothing else was runnable, so carry on" path, which schedule()'s own
 * comment explains it must take. The caller then re-tests its condition
 * and, if it is still unmet, blocks again. Without this cleanup that
 * second block would increment blocked_count a second time against a
 * single decrement, and the counter would climb until sched_wake_all's
 * fast path never fired again - a performance bug that looks like
 * nothing, degrades slowly, and would be miserable to attribute. */
static void unblock_self(task_t *self) {
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    if (self->state == TASK_BLOCKED) {
        self->state = TASK_RUNNING;
        blocked_count--;
    }
    self->wait_chan = (const void *)0;
    self->wake_deadline_ms = 0;
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

void sched_block_on_seq(const void *chan, uint64_t deadline_ms, uint64_t expected_seq) {
    sched_deliver_pending_signal(); /* noreturn if one is pending - see sched_block_on */

    int cpu = smp_current_cpu();
    uint64_t sflags = irq_save_disable();
    spin_lock(&sched_lock);
    if (event_seq != expected_seq) {
        /* Something happened while the caller was testing its condition.
         * Do not sleep on a world that has already changed - return and
         * let the caller test again. */
        spin_unlock(&sched_lock);
        irq_restore(sflags);
        return;
    }
    task_t *self = current_task[cpu];
    self->wait_chan = chan;
    self->wake_deadline_ms = deadline_ms;
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&sched_lock);
    irq_restore(sflags);

    schedule();
    unblock_self(self);
}

void sched_block_on(const void *chan, uint64_t deadline_ms, spinlock_t *lock, uint64_t *flags) {
    int cpu = smp_current_cpu();
    uint64_t sflags = irq_save_disable();
    spin_lock(&sched_lock);
    task_t *self = current_task[cpu];
    self->wait_chan = chan;
    self->wake_deadline_ms = deadline_ms;
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&sched_lock);
    irq_restore(sflags);

    spin_unlock_irqrestore(lock, *flags);

    /* THE SIGNAL CHECK GOES HERE, AFTER THE CALLER'S LOCK IS DOWN, AND
     * THE ORDERING IS NOT A STYLE CHOICE.
     *
     * sched_deliver_pending_signal is noreturn when a signal is pending:
     * it calls task_exit_with_code and this task never comes back. Doing
     * that one line earlier - while `lock` was still held - meant a task
     * killed at exactly this point died holding pipe_lock, with
     * interrupts off, and never released it. Every pipe operation on the
     * machine then spun on a lock whose owner no longer existed.
     *
     * That is precisely what happened, and it wedged the boot in the M36
     * self-test, which kills a client mid-conversation. The pre-M68 code
     * in pipe.c had the unlock and the signal check in this order for the
     * same reason; folding the check into this function quietly reversed
     * them. Worth recording because the bug is invisible in the diff -
     * both versions "check for a signal before sleeping", and only one of
     * them survives the check finding something.
     *
     * Marking this task BLOCKED above and then exiting here is harmless:
     * task_exit_with_code sets TERMINATED, which pick_next skips for its
     * own reasons. */
    sched_deliver_pending_signal();

    schedule();
    unblock_self(self);
    *flags = spin_lock_irqsave(lock);
}

void schedule(void) {
    int cpu = smp_current_cpu();
    /* See sched_lock's header comment: irq_save_disable is what keeps an
     * IPI_SCHEDULE_VECTOR interrupt landing on this same CPU from
     * reentering schedule() and deadlocking on a lock it already holds -
     * not optional hardening. `flags` is a plain local, so for the
     * "actually switches" path below it travels with `prev`'s own stack
     * and gets restored correctly whenever *that exact task* is next
     * resumed, regardless of which CPU or how much later. */
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    task_t *prev = current_task[cpu];
    task_t *next = pick_next(prev);

    /* M68: pick_next returns `from` when nothing else is READY. `prev` may
     * now be TASK_BLOCKED, and the obvious reaction - refuse to return
     * until something else becomes runnable - is WRONG, in a way that
     * cost this milestone a day and is worth writing down in full.
     *
     * schedule() is reached from two places: a task parking itself
     * (interrupts on), and scheduler_tick_cpu inside the timer IRQ
     * (interrupts off). sched_block_on marks the task BLOCKED, drops the
     * caller's lock, and only then calls schedule() - so there is a
     * window, a few instructions wide, where a task is BLOCKED and still
     * running. If the tick lands in that window and nothing else is
     * READY, a spin-until-something-is-ready loop here spins *inside the
     * interrupt handler*, where irq_restore puts back IF=0. Interrupts
     * off, forever, with the timer that would change the run queue being
     * the exact interrupt that can no longer fire. The machine is dead,
     * with no panic and no output - the same shape as the `hlt` M64
     * found, and `pause` instead of `hlt` does not help in the slightest,
     * because the problem was never the instruction.
     *
     * It presented as a boot that hung about one run in two, at whichever
     * self-test happened to have a task blocked when a tick arrived, and
     * it moved when anything at all changed the timing - including adding
     * the klog that was meant to diagnose it.
     *
     * So: return, exactly as before. A BLOCKED task that keeps running
     * for a few more instructions is harmless - it is on its way to
     * schedule() and will park there - and a BLOCKED task resumed by this
     * return simply re-tests its condition and blocks again, which is the
     * loop every caller of sched_block_on already has to have. The state
     * is transient by construction, and the only thing that could make it
     * permanent is a machine with genuinely nothing to run, where
     * spinning in the task's own context with interrupts ON is both
     * correct and the pre-M68 behaviour. */
    if (next == prev) {
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return;
    }

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    current_task[cpu] = next;

    tss_set_rsp0(cpu, next->kernel_stack_top);
    if (next->pml4_phys != loaded_pml4_phys[cpu]) {
        vmm_switch_address_space(next->pml4_phys);
        loaded_pml4_phys[cpu] = next->pml4_phys;
    }

    /* M63: save mine, load theirs, then switch - in that order, and the
     * order is the design rather than a detail. Restoring on the way
     * *back* would look symmetric and would leave a brand-new task
     * running on whatever the previous task left in the xmm registers,
     * because a fresh task never returns from context_switch at all (see
     * its own header comment). Loading the incoming task's state here
     * means every task starts from a state somebody chose. */
    fpu_save(prev->fpu_state);
    fpu_restore(next->fpu_state);

    /* sched_lock is still held here on purpose - see its own header
     * comment for why, and for exactly where/how it gets released once
     * `prev` (this exact call frame) is resumed. */
    context_switch(&prev->rsp, next->rsp);

    /* Resumed - possibly on a different physical CPU than the one that
     * started this switch (a task isn't pinned to the core that last ran
     * it), so re-derive rather than trust a cpu-id local captured before
     * the switch. Nothing else here needs `cpu` again, only the unlock,
     * which doesn't care which CPU performs it. `flags`, though, is
     * exactly the value *this* task's own call to irq_save_disable saved
     * above, before it was ever switched out - restoring it now (rather
     * than the resuming CPU's own state) is what correctly turns
     * interrupts back on for a voluntary caller while leaving them off
     * for one that called schedule() from inside its own interrupt
     * handler (scheduler_tick_cpu). */
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

void sched_deliver_pending_signal(void) {
    task_t *t = current_task[smp_current_cpu()];
    /* M76: any nonzero pending_signal, not just the two SYS_kill used to
     * accept. sched_raise_signal only ever sets this field for a signal
     * whose disposition on *this* task is death, so "a fatal signal is
     * pending" and "pending_signal != 0" are now the same statement.
     *
     * Leaving the old pair of name comparisons here is what made a
     * SIGINT sent to a process parked on the keyboard vanish: the task
     * was woken, came back round to this check, matched neither name,
     * and parked again - forever, and taking the boot with it. Worth
     * recording because the symptom (a machine that stops with no
     * message) is a long way from the cause (two enum values in a
     * condition that had been correct for sixty milestones). */
    if (t->pending_signal != 0) {
        deliver_pending_signal_and_exit(t); /* noreturn */
    }
}

void task_exit_with_code(int code) {
    task_t *t = current_task[smp_current_cpu()];
    /* Reclaims whatever shm segments this task created (kernel/ipc/
     * shm.h's own comment has the full "why" - MAX_SHM_SEGMENTS is small
     * and fixed, and nothing else ever frees one) before this id is gone
     * for good. Every exit path funnels through here - SYS_exit, a
     * SIGKILL/SIGTERM delivered either at the next syscall (syscall.c's
     * syscall_handler) or at the next timer tick (scheduler_tick_cpu,
     * below) - so this is the one place that's guaranteed to run exactly
     * once per task, right as it leaves the scheduler for good. */
    shm_free_by_owner(t->id);
    /* M59: and every descriptor it still holds. Until this milestone
     * there was nothing to give back - a pipe was not refcounted and
     * there were no file descriptors at all - so a task simply
     * disappeared with its table. Now dropping them is what tells a
     * reader that the last writer went away, which is the whole point of
     * the refcount. */
    sched_release_fds(t);
    /* M75: and its environment. Here rather than in sched_reap_slot for
     * the same reason the fd table is here: this is the one place
     * guaranteed to run exactly once per task, and a TERMINATED task that
     * nobody ever reaps would otherwise hold a page of heap for the life
     * of the machine. Nothing can read it after this point - a spawn is
     * something a *running* task does. */
    sched_release_env(t);

    /* M54: and the address space, which nothing has ever reclaimed - M29
     * documented the leak, M50 measured it at ~15 frames per dead
     * process, and both stopped there.
     *
     * The ordering is the whole risk, and it is handled by switching this
     * CPU to the kernel's own address space *first*. That is safe from
     * exactly here and nowhere else: the code executing is kernel code
     * and the stack under it is kernel heap, both of which live in
     * PML4[0] and are therefore identical in every address space - so the
     * switch changes nothing this function can observe, and the tables it
     * then frees are no longer loaded anywhere.
     *
     * loaded_pml4_phys is updated to match, or schedule() would skip the
     * CR3 reload for the next task on the belief that its address space
     * was already loaded. pml4_phys itself is repointed at the kernel's
     * so nothing can later try to switch to a table that has been freed. */
    /* ---- M79: whose address space is this, exactly? -------------------
     *
     * Until this milestone the answer was "this task's", because a task
     * was a process. A thread shares a page table with the task that
     * created it, so tearing it down here would pull the memory out from
     * under whichever of them happened to exit first.
     *
     * The rule is last-one-out, and `exiting` is what makes it safe: it
     * is set under sched_lock *before* the scan, so two threads leaving
     * at the same moment cannot each see the other as a live user and
     * both decline to free. The state cannot be set to TERMINATED this
     * early instead - another CPU would be free to reap the slot and
     * kfree the kernel stack this code is still running on. */
    if (t->pml4_phys != vmm_kernel_pml4_phys()) {
        int cpu = smp_current_cpu();
        uint64_t dead = t->pml4_phys;
        int others = 0;
        uint64_t eflags = irq_save_disable();
        spin_lock(&sched_lock);
        t->exiting = 1;
        for (int i = 0; i < task_count; i++) {
            task_t *o = &tasks[i];
            if (o == t || o->state == TASK_FREE || o->state == TASK_TERMINATED || o->exiting) {
                continue;
            }
            if (o->pml4_phys == dead) {
                others = 1;
                break;
            }
        }
        spin_unlock(&sched_lock);
        irq_restore(eflags);

        t->pml4_phys = vmm_kernel_pml4_phys();
        vmm_switch_address_space(t->pml4_phys);
        loaded_pml4_phys[cpu] = t->pml4_phys;
        if (!others) {
            process_destroy_address_space(dead);
        }
    }

    t->exit_code = code;
    if (t->state == TASK_BLOCKED) {
        blocked_count--; /* killed while parked - the count is not a state, it has to be maintained at every edge */
    }
    t->state = TASK_TERMINATED;
    /* M68: a parent blocked in SYS_wait parks on this exact task_t, and a
     * wait(-1) parks on the poll channel. Both are woken here, before the
     * final schedule() - after it there is no "here" to run in. */
    sched_wake_all((const void *)t);
    sched_wake_all(SCHED_POLL_CHAN);
    /* M76: and tell whoever spawned this task that it is gone. SYS_wait's
     * own ABI comment has named "more complete wait semantics" as
     * deferred since M14, and this is that deferral coming due: a parent
     * can now be *told* rather than having to poll SYS_task_alive on a
     * quarter-second timer the way init.c still does.
     *
     * Raised here rather than at reap time on purpose - the fact worth
     * reporting is "your child ended", and a parent that never reaps
     * would otherwise never hear it. sched_raise_signal drops it if the
     * parent installed no handler, which is SIGCHLD's default action and
     * therefore what every process on this machine that predates this
     * milestone gets: nothing at all. */
    if (t->parent_id >= 0) {
        task_t *parent = sched_task_by_id(t->parent_id);
        if (parent && parent != t) {
            sched_raise_signal(parent, SIGCHLD);
        }
    }
    schedule();
    /* Unreachable: a TERMINATED task is never picked again by pick_next,
     * so the context_switch inside that schedule() call never returns
     * here. This is a defensive backstop, not a real fallback path. */
    panic("task_exit: terminated task resumed");
}

void task_exit(void) {
    task_exit_with_code(0);
}

task_t *sched_current(void) {
    return current_task[smp_current_cpu()];
}

task_t *sched_task_by_id(int pid) {
    if (pid < 0) {
        return (task_t *)0;
    }
    int slot = PID_SLOT(pid);
    if (slot >= task_count) {
        return (task_t *)0;
    }
    task_t *t = &tasks[slot];
    /* M54: the generation check is the whole point - without it a pid
     * whose slot has since been recycled would answer about whoever holds
     * that slot now, which is a wrong answer that looks exactly like a
     * right one. */
    if (t->state == TASK_FREE || t->generation != PID_GEN(pid)) {
        return (task_t *)0;
    }
    return t;
}

task_t *sched_task_by_slot(int slot) {
    if (slot < 0 || slot >= task_count || tasks[slot].state == TASK_FREE) {
        return (task_t *)0;
    }
    return &tasks[slot];
}

int sched_task_count(void) {
    return task_count;
}

int sched_live_task_count(void) {
    int n = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_FREE) {
            n++;
        }
    }
    return n;
}

int sched_has_free_task_slot(void) {
    if (task_count < MAX_TASKS) {
        return 1;
    }
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE) {
            return 1;
        }
    }
    return 0;
}

void sched_reap_slot(task_t *t) {
    if (!t || t->state != TASK_TERMINATED) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    /* The kernel stack goes back rather than at exit, because at exit the
     * task was still running on it. By now it has been switched away from
     * for good - a TERMINATED task is never picked again, and the
     * incoming task's own release of sched_lock is what proves that
     * switch completed - so nothing is left pointing at it. 8 KiB a task,
     * which at the rate the boot self-tests spawn is worth more than the
     * slot itself.
     *
     * Detached here and freed *after* the unlock below: task_spawn_common
     * takes its heap lock before sched_lock precisely so the two are
     * never nested in the other order anywhere in this kernel, and
     * kfree-ing inside this critical section would be the one place that
     * broke it. */
    uint8_t *stack = t->stack_base;
    t->stack_base = NULL;
    t->kernel_stack_top = 0;
    t->generation++;
    t->state = TASK_FREE;
    t->pending_signal = 0;
    t->reaped = 0;
    t->parent_id = -1;
    t->caps = 0; /* M65: a free slot holds no authority, so a stale pointer to one cannot lend any */
    sched_reset_fds_to_std(t);
    /* M75: released at exit already; cleared here so a recycled slot can
     * never start life pointing at freed heap even if some future exit
     * path forgets. */
    t->env_block = NULL;
    t->env_len = 0;
    t->env_count = 0;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_restorer = 0;
    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = SIG_DFL_ADDR;
    }
    /* M78: the frames themselves went back with the address space in
     * task_exit_with_code; this is the bookkeeping that described them. */
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i].base = 0;
        t->mmaps[i].pages = 0;
    }
    t->is_thread = 0;
    t->exiting = 0;
    t->tgid = 0;
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    t->fds[0].type = FD_NONE; /* a free slot holds nothing at all, not even stdin/stdout */
    t->fds[1].type = FD_NONE;
    set_task_name(t, "");
    spin_unlock(&sched_lock);
    irq_restore(flags);
    if (stack) {
        kfree(stack);
    }
}

void sched_wake_task(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    event_seq++;
    if (t->state == TASK_BLOCKED) {
        blocked_count--;
        t->state = TASK_READY;
        t->prio = PRIO_INTERACTIVE;
        t->full_slices = 0;
        t->wait_chan = (const void *)0;
        t->wake_deadline_ms = 0;
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

task_t *sched_vm_owner(task_t *t) {
    if (!t || !t->is_thread) {
        return t;
    }
    task_t *leader = sched_task_by_id(t->tgid);
    /* A thread that has outlived its leader keeps its own bookkeeping
     * rather than dereferencing nothing. That state is then wrong in the
     * sense that a second thread would disagree with it - but a wrong
     * break is a bug in one program, and a null dereference here is the
     * machine. */
    return leader ? leader : t;
}

task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg) {
    /* Same page table as the leader - which is the whole milestone in
     * one argument. Heap and shm cursors are passed as zero because a
     * thread does not have its own: every one of those fields is read
     * through sched_vm_owner, which sends it to the leader's. */
    return task_spawn_common(name, leader->pml4_phys, entry, arg, 0, 0, leader);
}

int sched_count_sharing_address_space(uint64_t pml4_phys) {
    int n = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE || tasks[i].state == TASK_TERMINATED) {
            continue;
        }
        if (tasks[i].pml4_phys == pml4_phys) {
            n++;
        }
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
    return n;
}

void sched_kill_thread_group(task_t *t) {
    if (!t) {
        return;
    }
    int group = t->tgid;
    /* Collected under the lock and signalled after it: sched_raise_signal
     * takes sched_lock itself (through sched_wake_task), and this
     * kernel's one rule about that lock is that it is never taken twice. */
    task_t *victims[MAX_TASKS];
    int n = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    for (int i = 0; i < task_count; i++) {
        task_t *o = &tasks[i];
        if (o == t || o->state == TASK_FREE || o->state == TASK_TERMINATED) {
            continue;
        }
        if (o->tgid == group) {
            victims[n++] = o;
        }
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
    for (int i = 0; i < n; i++) {
        sched_raise_signal(victims[i], SIGKILL);
    }
}

void sched_raise_signal(task_t *t, int sig) {
    if (!t || t->state == TASK_TERMINATED || t->state == TASK_FREE) {
        return;
    }
    if (sig <= 0 || sig > SIG_MAX) {
        return;
    }
    if (!SIG_IS_CATCHABLE(sig)) {
        /* SIGKILL, and SIGSEGV if anything ever sends one. No handler,
         * no mask, no argument. */
        t->pending_signal = sig;
        sched_wake_task(t);
        return;
    }
    uint64_t h = t->sig_handler[sig];
    if (h == SIG_IGN_ADDR) {
        return;
    }
    if (h == SIG_DFL_ADDR) {
        /* The default action, which is death for everything here except
         * SIGCHLD. Being explicit about the one exception rather than
         * carrying a table: this kernel has one signal whose default is
         * to be ignored, and a table of one row is a table nobody reads. */
        if (sig == SIGCHLD) {
            return;
        }
        t->pending_signal = sig;
        sched_wake_task(t);
        return;
    }
    t->sig_pending |= (1u << sig);
    /* Woken even if the signal is currently blocked: the mask decides
     * when the handler *runs*, not whether the task should stop sleeping
     * - and a task that unblocks the signal a moment later would
     * otherwise sleep through it. */
    sched_wake_task(t);
}

int sched_signal_pending(void) {
    task_t *t = current_task[smp_current_cpu()];
    return (t->sig_pending & ~t->sig_blocked) != 0;
}

/* ---- M75: the environment a task hands to its children --------------
 *
 * Stored packed - `count` back-to-back NUL-terminated strings in `len`
 * bytes - rather than as an array of pointers, because that is the shape
 * both of its consumers want: proc.c walks it once to lay strings into
 * the child's argument region, and this function copies it wholesale.
 * An array of pointers would be a second allocation and a second thing
 * to free.
 */
void sched_release_env(task_t *t) {
    char *block = t->env_block;
    t->env_block = NULL;
    t->env_len = 0;
    t->env_count = 0;
    if (block) {
        kfree(block);
    }
}

int sched_set_env(task_t *t, const char *block, uint32_t len, uint32_t count) {
    if (!block || len == 0 || count == 0) {
        sched_release_env(t);
        return 0;
    }
    /* Allocated before the old one is dropped, so a failed copy leaves
     * the task with the environment it already had rather than with
     * none - the same "a failure should not also destroy what worked"
     * rule the rest of this kernel's replace paths follow. */
    char *copy = (char *)kmalloc(len);
    if (!copy) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        copy[i] = block[i];
    }
    sched_release_env(t);
    t->env_block = copy;
    t->env_len = len;
    t->env_count = count;
    return 0;
}

/* M59: the one place a descriptor stops being held. Before this
 * milestone "closing" an fd was blanking a slot and nothing else, because
 * neither of the two things a slot can point at was reference counted.
 * Both are now, and every path that drops a slot - SYS_close, a task
 * exiting, a slot being recycled, SYS_dup2 overwriting one - has to come
 * through here or the count is a lie. */
void fd_release(fd_slot_t *slot) {
    switch (slot->type) {
    case FD_PIPE_READ:
        pipe_unref_read(slot->pipe);
        break;
    case FD_PIPE_WRITE:
        pipe_unref_write(slot->pipe);
        break;
    case FD_FILE:
        openfile_unref(slot->file);
        break;
    case FD_SOCKET:
        socket_unref(slot->sock);
        break;
    default:
        break;
    }
    slot->type = FD_NONE;
    slot->pipe = (struct pipe *)0;
}

/* M59: the mirror image, for a slot being copied rather than dropped - a
 * spawned task inheriting its parent's whole table, and SYS_dup2. */
void fd_retain(const fd_slot_t *slot) {
    switch (slot->type) {
    case FD_PIPE_READ:
        pipe_ref_read(slot->pipe);
        break;
    case FD_PIPE_WRITE:
        pipe_ref_write(slot->pipe);
        break;
    case FD_FILE:
        openfile_ref(slot->file);
        break;
    case FD_SOCKET:
        socket_ref(slot->sock);
        break;
    default:
        break;
    }
}

void sched_release_fds(task_t *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_release(&t->fds[i]);
    }
}

void sched_reset_fds_to_std(task_t *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_release(&t->fds[i]);
    }
    t->fds[0].type = FD_STDIN;
    t->fds[1].type = FD_STDOUT;
}
