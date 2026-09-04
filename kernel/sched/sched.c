#include "sched.h"

#include "dev/pty.h" /* M85: a session leader that exits gives its terminal back */
#include "dev/tty.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/isr.h" /* M83: isr_regs_t - the frame a forked child resumes through */
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
#include "mm/pmm.h" /* M81: task stacks come from the frame allocator - see task_spawn */
#include "mm/vmm.h"
#include "panic.h"
#include "proc/proc.h" /* M54 - process_destroy_address_space, called from task_exit_with_code */
#include "lib/libk.h" /* M82: k_memset, zeroing a demand-filled page */
#include "mman.h"  /* system_api/include/mman.h - PROT_WRITE, read by sched_fault_fill */
#include "fs/vfs.h"     /* M91 (second attempt): a private file mapping reads the file */
#include "mm/filemap.h" /* M91 (second attempt): a shared one shares a frame */
#include "signal.h" /* system_api/include/signal.h - SIGKILL/SIGTERM */

/* M81: 8 KiB -> 32 KiB, and the reason is a number in another file.
 *
 * LEANFS_MAX_PATH went from 128 to 4096 this milestone, and M53's comment
 * next to the old value said exactly why that matters here: a path is put
 * on a kernel stack, "which is 8 KiB, so this being a number rather than
 * 'however long the caller's string is' is load-bearing rather than
 * tidy." It was right, and the first boot after the bump proved it - the
 * syscall layer holds two paths at once (copy_path_from_user's raw and
 * joined) while its caller already holds a third, and leanfs's
 * resolve_parent holds a fourth below that. Twelve kilobytes of paths on
 * an eight-kilobyte stack presents as a pmm_free_frame double-free during
 * boot, because what actually happened is that a task wrote through the
 * bottom of its stack into the allocator's business.
 *
 * 32 KiB is the deepest measured chain (about 12.5 KiB) with room for the
 * interrupt frames and the nested handlers a preemptible kernel (M67) can
 * stack on top of it. It costs 3 MiB across MAX_TASKS, which is a real
 * cost on a 128 MiB machine and a smaller one than the alternative:
 * pushing paths into per-task heap buffers would put an allocation in
 * front of every path-taking syscall. */
#define TASK_STACK_SIZE (32 * 1024)
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

/* M106 - see sched_peak_live_tasks, defined with the other accessors. */
static int peak_live_tasks;
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
void sched_dump_cpus(void); /* M106 - defined below, used by the switch check */
static void wake_expired(uint64_t now_ms);
static void fire_expired_alarms(uint64_t now_ms); /* M89 */
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
/* M106: deleted, and the deletion is the fix.
 *
 * This counter was incremented by sched_idle_enter on the CPU the task
 * was running on and decremented by sched_idle_exit on the CPU it was
 * running on *when it woke up*. Those are the same CPU only if the task
 * was never migrated while it slept - which on one core is always, and on
 * four is a coin flip. Every migration across a sleep leaked one count
 * onto the entering CPU and swallowed one on the leaving CPU (the
 * decrement is floor-guarded at zero), so within a few seconds of boot
 * some CPU's depth was permanently above zero and every tick it took was
 * counted as idle. The machine reported cores that were busy as asleep.
 *
 * The per-task counter M101 added right beside it has no such problem -
 * it travels with the task, which is the thing that is actually waiting -
 * so the per-CPU question "was this CPU idle on this tick" is answered by
 * asking the task this CPU is running. One counter, owned by the thing it
 * describes, and no way for a migration to desynchronise it from
 * anything. */

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
    /* One smp_current_cpu() for both, not two. It reads the Local APIC's
     * ID register over MMIO and then scans the CPU table, which is not
     * the sort of thing to do twice in a row on a path this kernel takes
     * hundreds of times a boot - and M101's own profile is what made
     * that cost visible. */
    int cpu = smp_current_cpu();
    /* M106: the same bracket, recorded on the task alone. The per-CPU
     * counter this used to raise beside it is gone - see above. The
     * comment below is M101's and is kept because it is what explains why
     * the per-task one is the right one: "is THIS task waiting
     * halted"; a task can be scheduled away from inside the bracket, so
     * only the per-task one answers "is THIS task waiting". Both are
     * kept because they are different questions - see task_t's field. */
    task_t *self = current_task[cpu];
    if (self && self->idle_wait_depth < 255) {
        self->idle_wait_depth++;
    }
    irq_restore(flags);
}

/* M101. Read without a lock: the only caller is the timer interrupt,
 * looking at the task it just interrupted on its own CPU, and that task
 * cannot be running anywhere else to change the value. */
int sched_task_is_idle_waiting(const task_t *t) {
    return t && t->idle_wait_depth > 0;
}

void sched_idle_exit(void) {
    uint64_t flags = irq_save_disable();
    int cpu = smp_current_cpu();
    task_t *self = current_task[cpu];
    if (self && self->idle_wait_depth > 0) {
        self->idle_wait_depth--;
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
    cpu_enable_interrupts();
    for (;;) {
        cpu_halt();
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

/* M88: see sched.h. No lock: this runs on every timer tick on every core,
 * and the only writer of a given task's counters is the core that is
 * running it, so the race a lock would close cannot happen. A reader on
 * another core can see a count one tick stale, which is a distinction
 * nothing reporting centiseconds can act on.
 *
 * The idle identities are charged too, deliberately. They are tasks; a
 * `times()` asked about one would otherwise report a process that has
 * been alive for an hour and used no CPU. M68's idle_ticks answers "was
 * this machine busy" and this answers "where did this task's time go" -
 * two questions, two counters, rather than one counter asked to be both. */
void sched_account_tick(int user) {
    task_t *t = current_task[smp_current_cpu()];
    if (!t) {
        return;
    }
    if (user) {
        t->user_ticks++;
    } else {
        t->sys_ticks++;
    }
}

/* M45: bounded copy into a task_t's own fixed name buffer. NULL and an
 * over-long name are both ordinary inputs here, not errors - see
 * TASK_NAME_MAX's own comment in sched.h. */
static void set_task_name(task_t *t, const char *name);

/* M84: exposed as sched_set_task_name, because execve renames a task that
 * already exists - the one caller outside this file, and the one case
 * where the name a person sees in the task manager has to follow the
 * program actually running rather than the one that was spawned. */
void sched_set_task_name(task_t *t, const char *name) {
    set_task_name(t, name);
}

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

/* M83: the same resume point, for a task that has no entry function to
 * call. Unlocks sched_lock exactly as the trampoline above does - that
 * pairing is the invariant, not which trampoline does it - and then hands
 * the frame at the top of this task's own kernel stack to
 * fork_return_to_user, which pops it and `iretq`s into ring 3. Never
 * returns: from here on this task is a user process that happens to have
 * been born in the middle of a syscall. */
extern void fork_return_to_user(void *frame) __attribute__((noreturn));

static void fork_child_trampoline(void) __attribute__((noreturn));
static void fork_child_trampoline(void) {
    spin_unlock(&sched_lock);
    task_t *t = current_task[smp_current_cpu()];
    fork_return_to_user((void *)(t->kernel_stack_top - sizeof(isr_regs_t)));
}

/* Terminates a task in response to a pending fatal signal (SIGKILL or
 * SIGTERM - the only two this project recognizes, both with the same
 * "just terminate" default action). Exit code follows the standard
 * shell convention (128 + signal number) so SYS_wait's caller can tell a
 * signal death from a normal exit. */
/* M85: put a stopped task back on the run queue.
 *
 * Not sched_wake_task, which only ever moves TASK_BLOCKED - and
 * deliberately so: every wake in this kernel goes through that function
 * and none of them should be able to resume a suspended process by
 * accident. Resuming is a decision, and this is the one function that
 * makes it. */
void sched_resume_stopped(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    if (t->state == TASK_STOPPED) {
        t->state = TASK_READY;
        t->prio = PRIO_INTERACTIVE;
        t->full_slices = 0;
        t->wait_chan = (const void *)0;
        t->wake_deadline_ms = 0;
        /* M85: it is not stopped any more, so there is no stop left to
         * report. A parent that had not yet asked must not be told about
         * one that is over. */
        t->stopped_sig = 0;
        t->stop_reported = 0;
        event_seq++;
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

/* M85: stop the *current* task, at a point where stopping is safe.
 *
 * Called from the same two places a fatal pending signal is taken - the
 * scheduler tick and the syscall boundary - because a stop, like a death,
 * has to happen to a task that is not in the middle of something. Marks
 * the task TASK_STOPPED and reschedules; pick_next's existing "is it
 * TASK_READY" test does the rest, and nothing will pick it again until
 * sched_resume_stopped says so. */
static void take_pending_stop(task_t *t) {
    int sig = t->pending_stop;
    t->pending_stop = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    t->state = TASK_STOPPED;
    /* M85: the fact, kept for a parent in waitpid(WUNTRACED). Set under
     * the lock beside the state so a parent can never see one without
     * the other. */
    t->stopped_sig = sig;
    t->stop_reported = 0;
    event_seq++;
    spin_unlock(&sched_lock);
    irq_restore(flags);
    /* A parent blocked on this child, or on the poll channel waiting for
     * any child, has to be woken - a stop is an event a wait reports,
     * and until M85 the only such event was a death. Both channels,
     * because waitpid parks on the child for a specific pid and on the
     * poll channel for -1. */
    sched_wake_all((const void *)t);
    sched_wake_all(SCHED_POLL_CHAN);
    schedule();
}

void sched_take_pending_stop_if_any(task_t *t) {
    if (t && t->pending_stop != 0) {
        take_pending_stop(t);
    }
}

static void deliver_pending_signal_and_exit(task_t *t) __attribute__((noreturn));
static void deliver_pending_signal_and_exit(task_t *t) {
    int sig = t->pending_signal;
    t->pending_signal = 0;
    task_exit_with_signal(sig);
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
    if (t->is_idle || sched_task_is_idle_waiting(t)) {
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
    /* M89: and the alarms, after the wakes - see fire_expired_alarms. */
    fire_expired_alarms(pit_get_ticks() * (1000 / PIT_HZ));

    /* M76: any nonzero pending_signal at all, not just the two that used
     * to be the only ones SYS_kill would accept. sched_raise_signal is
     * now the only thing that sets this field, and it only ever sets it
     * for a signal whose *disposition on this task* is death - a caught
     * one goes in sig_pending instead. So "there is a fatal signal
     * pending" and "pending_signal != 0" became the same statement. */
    if (t->pending_signal != 0) {
        deliver_pending_signal_and_exit(t);
    }
    /* M85: and a stop, at the same point and for the same reason. After
     * the death check, because a task with both pending is a task that
     * has been killed - and stopping it first would leave it suspended
     * with a SIGKILL it can never take. */
    if (t->pending_stop != 0) {
        take_pending_stop(t);
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
    /* M106: whichever CPU this interrupt actually landed on, not a
     * hardcoded 0. IRQ 0 does reach the boot CPU on both interrupt
     * controllers this kernel drives - the 8259 has nowhere else to send
     * it, and M103's I/O APIC routes it to the boot CPU's LAPIC on
     * purpose - so the constant was true. It was true by a fact stated in
     * two other files, about a path where being wrong means charging one
     * core's tick to another core's task and demoting it on another
     * core's behalf. `str` costs one instruction and cannot be wrong. */
    scheduler_tick_cpu(smp_current_cpu());
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
    tasks[0].sid = 0;  /* M85: the boot task is session 0, and everything descends from it */
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
    /* M98: the same scan-then-grow task_spawn does, where a bare
     * `task_count` stood. On the machine the APs come up before
     * userland can fill the table and the bare index was never wrong -
     * but it was never CHECKED either, and the host harness proved it
     * writes past `tasks[]` the moment the table has been full first.
     * An AP with no slot to be is a panic rather than a corruption:
     * there is no machine worth running whose idle identities do not
     * fit. */
    int slot = -1;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (task_count >= MAX_TASKS) {
            panic("sched_init_ap: no task slot for a CPU's idle identity");
        }
        slot = task_count++;
    }
    task_t *t = &tasks[slot];
    t->state = TASK_RUNNING;
    /* The slot's existing generation, not 0: a reused slot resetting it
     * would let a stale pid name this idle identity (M98, with the slot
     * scan above). A never-used slot has generation 0 anyway, which is
     * what keeps a first boot's ids readable. */
    t->id = PID_MAKE(slot, t->generation);
    t->stack_base = NULL; /* this is ap_main's own boot stack (smp.c's start_ap kmalloc'd it), not one this table owns or will ever free */
    t->kernel_stack_top = 0; /* like task 0, never consulted - this idle identity never enters ring 3 */
    t->pml4_phys = vmm_kernel_pml4_phys();
    fpu_state_init(t->fpu_state); /* M63: this AP's idle identity, same reason as task 0's */
    t->fds[0].type = FD_STDIN;
    t->fds[1].type = FD_STDOUT;
    t->parent_id = -1;
    t->pgid = 0;
    t->sid = 0;  /* M85 - an idle identity is in the boot session like task 0 */
    t->pending_signal = 0;
    t->reaped = 0;
    t->exit_signal = 0; /* M84: not killed until something kills it */
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
    /* task_count moved into the slot scan above (M98) - counting here as
     * well double-counted a reused slot. */
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
    /* M81: a kernel stack comes from the frame allocator, not the heap.
     *
     * It was kmalloc'd for sixty milestones and that was fine while it was
     * 8 KiB. At 32 KiB it stopped being fine, and the way it announced
     * that is worth recording: the M67 concurrency self-test began
     * reporting nineteen leaked frames. Nothing had leaked. Four
     * concurrent 32 KiB stacks were more than the heap had slack for, so
     * the heap grew - and a heap here grows by taking frames from the PMM
     * and never gives them back, which is indistinguishable from a leak to
     * a test that counts free frames on either side of a spawn.
     *
     * A stack is a page-granular object with a page-granular lifetime,
     * which is exactly what pmm_alloc_contiguous is for. Taking it from
     * there means the frames a task borrows are the frames it returns, and
     * the self-test's accounting balances because it is now telling the
     * truth. It also stops task stacks fragmenting a heap they have no
     * business being in.
     *
     * Allocated before sched_lock for the same lock-ordering reason the
     * kmalloc it replaces was (see heap.c's note): the PMM takes its own
     * lock, and these two are never nested in the other order anywhere. */
    _Static_assert(TASK_STACK_SIZE % 4096 == 0, "a kernel stack must be a whole number of frames");
    /* M102: try_, so the check below is reachable. It was written
     * against pmm_alloc_contiguous, which panics rather than returning 0,
     * so it has been dead code since it was written - the same shape of
     * dead guard this milestone found in proc.c's argument frames. */
    uint8_t *stack_base = (uint8_t *)(uintptr_t)pmm_try_alloc_contiguous(TASK_STACK_SIZE / 4096);
    if (!stack_base) {
        return (task_t *)0;
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
            pmm_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
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
        /* M84: a spawn is a fork and an exec in one call, and FD_CLOEXEC
         * is about the exec half - so a descriptor marked close-on-exec
         * does not reach the program this starts. Dropped rather than
         * copied-then-closed: the slot is never retained, so there is no
         * reference to give back and no window in which the child holds
         * something it was never meant to. */
        if (t->fds[i].cloexec) {
            t->fds[i].type = FD_NONE;
            t->fds[i].cloexec = 0;
            continue;
        }
        fd_retain(&t->fds[i]); /* M59: the child holds these too - see fd_retain */
    }
    t->parent_id = caller->id;
    t->pgid = caller->pgid;
    t->sid = caller->sid; /* M85: a child stays in its parent's session until setsid says otherwise */
    t->pending_signal = 0;
    t->reaped = 0;
    t->exit_signal = 0; /* M84: not killed until something kills it */
    /* M88: a slot may be a reaped task's, and CPU time belongs to the
     * occupant rather than to the slot. Zeroed inside the same critical
     * section that publishes this task as READY, for the reason this
     * function's own M40 note gives: it is schedulable the instant the
     * lock drops, so the first tick to land on it would otherwise be
     * added to the previous occupant's total. */
    t->user_ticks = 0;
    t->sys_ticks = 0;
    t->child_user_ticks = 0;
    t->child_sys_ticks = 0;
    t->max_rss_pages = 0;        /* M98 */
    t->child_max_rss_pages = 0;  /* M98 */
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
    t->sig_siginfo = 0; /* M99 - reset with the handlers it describes */
    t->si_pid = 0;
    t->si_status = 0;
    t->si_addr = 0;
    /* M78: an empty arena. Not inherited for the same reason the heap
     * cursor above is not: this is a fresh address space, so every
     * address the parent had mapped means nothing here. */
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i].base = 0;
        t->mmaps[i].pages = 0;
    }
    /* M96: and the thread pointer, which is per TASK and must not be
     * inherited by whoever gets this slot next.
     *
     * Found by M95: a recycled slot kept the previous task's fs_base,
     * libc's TLS setup saw a thread pointer already set and stood down
     * (which is exactly what it should do in a dynamic program - see
     * tls.c), and the new process then read `errno` through a pointer
     * into a dead address space. It presented as a null-ish page fault
     * in whatever function first touched a __thread variable, three
     * subsystems away from the cause. */
    t->fs_base = 0;
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
    /* ---- M106: and a clean scheduling class -------------------------
     *
     * `prio`, `full_slices` and `last_block_tick` were never set here.
     * A slot is recycled, and M69 demotes a task that burns its slices
     * without blocking to PRIO_BATCH - so a slot whose last occupant was
     * a compute job handed the next occupant that demotion at birth.
     * pick_next only returns a PRIO_BATCH task when nothing interactive
     * wants a CPU at all, so such a task can sit TASK_READY behind a
     * poll loop indefinitely.
     *
     * Found by M55's self-test on two cores, immediately after M54's 384
     * spawn/reap rounds had left the table full of slots demoted by
     * exactly that rule: two freshly spawned window clients were READY
     * for five seconds and never ran once. Every other field here that a
     * recycled slot could poison - the signal table, the mmap arena,
     * fs_base, the CPU-time counters - was found the same way, by
     * something that started life holding a dead task's state, and each
     * has its own note above saying so. This is the sixth.
     *
     * INTERACTIVE rather than BATCH as the starting class, and the
     * asymmetry is deliberate: M69's demotion is evidence a task gathers
     * about itself by running, and a task that has never run has none. */
    t->prio = PRIO_INTERACTIVE;
    t->full_slices = 0;
    t->last_block_tick = pit_get_ticks();

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

    /* M106 - see sched_peak_live_tasks. Inside the same critical section
     * that publishes the task, so the count includes it. */
    {
        int live = 0;
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].state != TASK_FREE) {
                live++;
            }
        }
        if (live > peak_live_tasks) {
            peak_live_tasks = live;
        }
    }

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
/* M98: is this task on some CPU's stack right now? Under sched_lock the
 * answer is exact for a task that is INSIDE schedule() - M106's reap fix
 * says why at length - but there is a window it does not cover: a task
 * that has marked itself TASK_BLOCKED on the way into a wait and has not
 * yet reached schedule() is still current on its CPU, still standing on
 * its own kernel stack, and already wakeable. A futex_wake from another
 * core in that window makes it TASK_READY, and a pick_next that looks
 * only at state then hands the SAME task - the same kernel stack - to a
 * second CPU. Found by [m97]'s std::thread fixture on four cores: the
 * switch-away guard caught cpu0 standing on the stack of the thread
 * cpu1 was running. The task is not lost by being skipped - the CPU it
 * is still current on is inside (or one instruction from) schedule(),
 * and whoever scans next picks it up the moment it stops being current. */
static int current_on_some_cpu(const task_t *t) {
    for (int c = 0; c < MAX_CPUS; c++) {
        if (current_task[c] == t) {
            return 1;
        }
    }
    return 0;
}

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
        /* M98: READY is necessary but no longer sufficient - see
         * current_on_some_cpu above. `from` excludes itself via the
         * fallback below, which is the one legitimate "still current
         * here, keep running it" case. */
        if (&tasks[i] != from && current_on_some_cpu(&tasks[i])) {
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
/* ---- M89: alarm(2) ---------------------------------------------------
 *
 * A per-task deadline that raises SIGALRM when it passes. This is the
 * first timer in this kernel that is not a *sleep* - a sleep blocks the
 * task that asked and ends by making it runnable, and an alarm leaves it
 * running and interrupts it later.
 *
 * Checked once per tick, from the same place `wake_expired` is called
 * and immediately after it, so the two share one pass over the task
 * table and one lock acquisition. That ordering is deliberate: a task
 * whose sleep and whose alarm expire on the same tick should wake and
 * then see the signal, not be signalled while still blocked.
 *
 * The resolution is a tick - 10 ms at PIT_HZ 100 - which is stated in
 * <unistd.h> rather than rounded up to make a nicer number. `alarm(1)`
 * fires between 1.00 and 1.01 seconds from now, and a program that needs
 * better than that is asking this machine for something it does not
 * measure.
 *
 * There is exactly one alarm per task, which is what alarm(2) is: a
 * second call replaces the first and returns what was left of it. No
 * setitimer, no timer_create, no per-thread alarms - each of those is a
 * real feature and none of them is what a program calling alarm() for a
 * timeout wants.
 */
static void fire_expired_alarms(uint64_t now_ms) {
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    int to_signal[MAX_TASKS];
    int n = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_FREE && tasks[i].state != TASK_TERMINATED &&
            tasks[i].alarm_deadline_ms != 0 &&
            now_ms >= tasks[i].alarm_deadline_ms) {
            tasks[i].alarm_deadline_ms = 0;
            to_signal[n++] = tasks[i].id;
        }
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
    /* Raised with sched_lock DROPPED: sched_raise_signal takes it, and a
     * kernel with two lock orders is a kernel with a deadlock in it (see
     * openfile_unref for the same move and the same reason). The gap is
     * unobservable - the deadline is already cleared, so no second pass
     * can raise the same alarm twice. */
    for (int i = 0; i < n; i++) {
        task_t *t = sched_task_by_id(to_signal[i]);
        if (t) {
            sched_raise_signal(t, SIGALRM);
        }
    }
}

unsigned int sched_set_alarm(task_t *t, unsigned int seconds) {
    if (!t) {
        return 0;
    }
    uint64_t now_ms = pit_get_ticks() * (1000 / PIT_HZ);
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    unsigned int remaining = 0;
    if (t->alarm_deadline_ms > now_ms) {
        /* POSIX rounds the remainder UP: a caller told "0 seconds left"
         * on an alarm that has not fired would cancel a timer it thinks
         * is already done. */
        remaining = (unsigned int)((t->alarm_deadline_ms - now_ms + 999) / 1000);
    }
    t->alarm_deadline_ms = seconds ? now_ms + (uint64_t)seconds * 1000 : 0;
    spin_unlock(&sched_lock);
    irq_restore(flags);
    return remaining;
}

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

/* M96: wake at most `max` waiters on `chan`, and say how many.
 *
 * FUTEX_WAKE(1) is what a mutex unlock passes, and waking every waiter
 * for it is the thundering herd a futex exists to avoid: N threads wake,
 * N-1 find the lock taken and sleep again, and the cost is N context
 * switches per handoff rather than one.
 *
 * sched_wake_all is left exactly as it was rather than being expressed
 * in terms of this: its callers - a pipe, a socket, the keyboard - all
 * genuinely mean everybody, and reading `sched_wake_n(chan, INT_MAX)` at
 * those sites would be a worse way to say so. */
int sched_wake_n(const void *chan, int max) {
    if (!chan || max <= 0) {
        return 0;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    event_seq++; /* the same reason sched_wake_all bumps it - see below */
    int woken = 0;
    for (int i = 0; i < task_count && woken < max; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            tasks[i].prio = PRIO_INTERACTIVE; /* M69 - see sched_wake_all */
            tasks[i].full_slices = 0;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
            woken++;
        }
    }
    if (woken > 0) {
        need_resched[smp_current_cpu()] = 1;
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
    return woken;
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
    /* M85 (second attempt): and a job-control stop, before parking again.
     *
     * This is the third checkpoint and the one that makes ^Z work on a
     * program that is waiting for input - which is nearly every program
     * anybody types ^Z at. The other two are the scheduler tick and the
     * syscall boundary, and neither reaches this case: a task blocked in
     * a read is not current when the tick fires, and it is already
     * *inside* the syscall, so it never crosses the boundary again. It
     * wakes, finds nothing to read, and parks - here - forever.
     *
     * Safe from exactly this position: the caller of this function holds
     * no lock (that is what distinguishes it from sched_block_on) and
     * has not yet marked itself blocked, so a stop taken here is a stop
     * taken by a task that is simply running.
     *
     * sched_block_on's callers DO hold a lock and are not covered.
     * Written down rather than left implicit: the paths that matter for
     * job control - a terminal read, a poll, a wait - all come through
     * this function, and pipe_read is the one that does not. A pipeline
     * stage blocked on a pipe takes its stop at the next tick that finds
     * it current, which is where M85's first attempt left everything. */
    sched_take_pending_stop_if_any(current_task[smp_current_cpu()]);

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

    /* ---- M106: two CPUs must never be on one stack -----------------
     *
     * The invariant this kernel's whole SMP design rests on, and the one
     * nothing checked: a task is RUNNING on at most one CPU. Everything
     * else follows from it - a task's kernel stack, its saved rsp, its
     * FPU area and its TSS rsp0 are all single-owner state, and two cores
     * resuming one task means two cores executing on one stack, which
     * presents as a garbage rip in low memory and rflags with NT and DF
     * set. That is exactly what the first four-core boot produced.
     *
     * Checked rather than argued because it costs MAX_CPUS compares under
     * a lock that is already held, on a path that is already doing a
     * CR3 reload and an FPU save. If it ever fires it names the two CPUs,
     * which is a diagnosis rather than a symptom. */
    for (int c = 0; c < MAX_CPUS; c++) {
        if (c != cpu && current_task[c] == next) {
            klog_puts("[sched] cpu ");
            klog_put_dec((uint32_t)cpu);
            klog_puts(" picked task '");
            klog_puts(next->name[0] ? next->name : "(unnamed)");
            klog_puts("' which cpu ");
            klog_put_dec((uint32_t)c);
            klog_puts(" is already running\n");
            panic("sched: one task, two CPUs - a stack with two owners");
        }
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

    /* M96: and the thread pointer, in the same place and for the same
     * reason. `%fs:0` is where every access to a `__thread` variable
     * goes; a task resumed with the previous task's FS base reads the
     * previous task's copy. Written unconditionally rather than only
     * when it differs - a compare is a memory read and a wrmsr is about
     * as cheap, and "only when it differs" is how a stale value survives
     * a path somebody adds later. */
    cpu_write_msr(MSR_FS_BASE, next->fs_base);

    /* ---- M106: is this CPU standing where it thinks it is? ---------
     *
     * context_switch writes the CURRENT stack pointer into prev->rsp. If
     * `prev` is not the task whose stack this CPU is actually on, that
     * write puts one task's resume point inside another task's stack, and
     * the machine dies later, somewhere else, in a way that looks like a
     * scheduler bug one step removed from wherever it started. Three
     * boots of chasing exactly that is why this is here.
     *
     * Only for a task with a stack this table owns: task 0 is on the boot
     * stack and an AP's cpu-idle identity is on its own boot stack, and
     * neither is described by kernel_stack_top (both are 0, deliberately
     * - see sched_init_ap). */
    if (prev->kernel_stack_top != 0) {
        uint64_t sp = cpu_stack_pointer();
        uint64_t base = (uint64_t)(uintptr_t)prev->stack_base;
        if (sp < base || sp >= prev->kernel_stack_top) {
            klog_puts("[sched] cpu ");
            klog_put_dec((uint32_t)cpu);
            klog_puts(" is switching away from '");
            klog_puts(prev->name[0] ? prev->name : "(unnamed)");
            klog_puts("' (stack 0x");
            klog_put_hex64(base);
            klog_puts("..0x");
            klog_put_hex64(prev->kernel_stack_top);
            klog_puts(") while standing on rsp=0x");
            klog_put_hex64(sp);
            klog_putc('\n');
            sched_dump_cpus();
            panic("sched: this CPU is not on the stack of the task it thinks it is running");
        }
    }

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

/* M106: every CPU's idea of what it is running, printed beside a fault.
 * A fault report on four cores that names only the faulting core cannot
 * distinguish "this task went wrong" from "this CPU's view of the world
 * went wrong", and those have completely different causes. Reads without
 * the lock deliberately: it is called from a fault handler, where taking
 * a lock another core may hold is how a diagnostic turns into a hang. */
void sched_dump_cpus(void) {
    for (int c = 0; c < MAX_CPUS; c++) {
        task_t *t = current_task[c];
        if (!t) {
            continue;
        }
        klog_puts("  cpu");
        klog_put_dec((uint32_t)c);
        klog_puts("=");
        klog_puts(t->name[0] ? t->name : "(unnamed)");
        klog_puts("/0x");
        klog_put_hex32((uint32_t)t->id);
        klog_puts(" kstack=0x");
        klog_put_hex64(t->kernel_stack_top);
    }
    klog_putc('\n');
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

void task_exit_with_signal(int sig) {
    task_t *t = current_task[smp_current_cpu()];
    if (t) {
        t->exit_signal = sig;
    }
    task_exit_with_code(128 + sig);
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

    /* M85 (second attempt): a session leader that exits hands back its
     * controlling terminal, and its foreground job gets a SIGHUP.
     *
     * Here for the same reason everything above is: this runs exactly
     * once per task. Missing it is not a leak, it is a terminal that
     * cannot be claimed again - the second program to run in a pty is
     * refused by a session that no longer exists, which is precisely how
     * this was found. Both the console and every pty are asked, because
     * which terminal a session owns is a fact about the terminal rather
     * than about the task. */
    if (t->sid != 0 && t->sid == t->id) {
        tty_release_session(tty_console(), t->sid);
        pty_release_session(t->sid);
    }

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
     * free the kernel stack this code is still running on. */
    if (t->pml4_phys != vmm_kernel_pml4_phys()) {
        int cpu = smp_current_cpu();
        uint64_t dead = t->pml4_phys;
        /* M98: take the peak off the address space before anything can
         * free it. This is one of the two places the number can be
         * captured at all - the other is execve, which throws an address
         * space away without the process ending - and after this line
         * the count belongs to a table entry that is about to be
         * released. Read before sched_lock is taken: it acquires the
         * VMM's lock, and the order everywhere else in this kernel is
         * VMM-then-scheduler or neither. */
        uint64_t peak = vmm_rss_peak_pages(dead);
        if (peak > t->max_rss_pages) {
            t->max_rss_pages = peak;
        }
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

        /* M91 (second attempt): shared file pages go back to filemap
         * before the address space is torn down, and only when this is
         * the last thread in it - a sibling still running is still
         * reading them. Done before the CR3 switch below, because
         * vmm_user_range_ok and vmm_unmap_page_in work on a pml4 by
         * address and `dead` is still this task's. */
        if (!others) {
            sched_release_shared_range(t, USER_MMAP_BASE, USER_MMAP_LIMIT);
        }
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
            /* M99: which child, and how it went, recorded before the
             * signal is raised so an SA_SIGINFO handler has something
             * true to read. The status is wait()'s encoding, which is
             * what si_status means and what a handler comparing against
             * CLD_EXITED is about to do arithmetic on. */
            parent->si_pid = (int32_t)t->id;
            parent->si_status = t->exit_signal ? (int32_t)(t->exit_signal & 0x7F)
                                               : (int32_t)(t->exit_code & 0xFF);
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

/* ---- M106: the two ceilings, watched rather than guessed ------------
 *
 * M106's own bullet asks for MAX_TASKS and MAX_FDS "raised to whatever a
 * parallel build actually asks for" and adds "the new numbers are set by
 * the failure, not by rounding up". There is no failure to set them by:
 * nothing on this machine has ever come close to either. So instead of
 * rounding up, the peaks are recorded and reported on every graded boot,
 * and the day something does approach a ceiling the number says so.
 *
 * The task peak is maintained at spawn, where a table walk is already
 * cheap next to an address space. The fd high-water is a walk of the
 * whole table and is asked for only at the marker. */
int sched_peak_live_tasks(void) {
    return peak_live_tasks;
}

int sched_fd_high_water(int *which_task_out) {
    int best = 0;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE) {
            continue;
        }
        int used = 0;
        for (int k = 0; k < MAX_FDS; k++) {
            if (tasks[i].fds[k].type != FD_NONE) {
                used++;
            }
        }
        if (used > best) {
            best = used;
            if (which_task_out) {
                *which_task_out = tasks[i].id;
            }
        }
    }
    spin_unlock_irqrestore(&sched_lock, f);
    return best;
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
    /* ---- M106: wait until it is actually off its stack ---------------
     *
     * task_exit_with_code marks a task TERMINATED and wakes its parent
     * BEFORE calling schedule(), so on more than one core the parent can
     * arrive here while the corpse is still executing on the kernel stack
     * this function is about to hand back to the frame allocator. What
     * that produced is in M106's notes: two contexts on one stack, and a
     * context_switch that popped somebody else's frame.
     *
     * "Has it left its stack" is asked directly rather than tracked with
     * a flag, and the second attempt is why. The first kept an `on_cpu`
     * bit handed from the outgoing task to the incoming one across every
     * switch; it leaked - a task ended up TERMINATED, current on no CPU,
     * and still marked on - and a flag that can be wrong about this is
     * worse than no flag, because the thing it guards is a stack.
     *
     * The exact question is "is this task current on any CPU", and
     * sched_lock is what makes the answer exact. A task stops being
     * current only inside schedule(), which holds this lock across the
     * switch - so a corpse that is not current while we hold the lock
     * cannot be mid-switch either. The lock is taken and dropped around
     * each attempt rather than held across the wait, because the corpse's
     * own route off its stack goes through schedule(), which needs it.
     */
    uint64_t flags;
    {
        uint64_t deadline = pit_get_ticks() + PIT_HZ; /* 1s - a ceiling, not a guess */
        for (;;) {
            flags = irq_save_disable();
            spin_lock(&sched_lock);
            int still_running = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                if (current_task[c] == t) {
                    still_running = 1;
                    break;
                }
            }
            if (!still_running) {
                break; /* the lock stays held - the reap body runs under it */
            }
            spin_unlock(&sched_lock);
            irq_restore(flags);
            if (pit_get_ticks() > deadline) {
                klog_puts("[sched] '");
                klog_puts(t->name[0] ? t->name : "(unnamed)");
                klog_puts("' pid 0x");
                klog_put_hex32((uint32_t)t->id);
                klog_puts(" is terminated and still on a CPU\n");
                sched_dump_cpus();
                panic("sched_reap_slot: a terminated task never left its kernel stack");
            }
            cpu_spin_hint();
        }
    }
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
    /* M88: the child's CPU time moves to the parent before the slot is
     * cleared, which is what `times()`' tms_cutime and tms_cstime are.
     *
     * Here rather than in the two wait paths in syscall.c because this is
     * the single point every reap goes through - sys_wait reaps in two
     * places and sys_waitpid in two more, and a fifth would have been
     * added silently the next time somebody wrote one. A child's own
     * accumulated child-time comes along with it: a grandchild's ticks
     * are reported once, at the generation that waited for them, exactly
     * as POSIX specifies.
     *
     * sched_task_by_id takes no lock, so calling it while holding
     * sched_lock is safe - it is a bounds check and a generation compare
     * over a table this critical section already owns. */
    task_t *parent = sched_task_by_id(t->parent_id);
    if (parent) {
        if (t->is_thread && t->tgid == parent->tgid) {
            /* A THREAD of the same process, not a child of it - and the
             * distinction is the whole of `times()`' contract. A thread's
             * CPU time is time this process spent; a child's is not.
             *
             * The first version of this did not make the distinction,
             * because a thread is a task with a parent_id like any other,
             * and libctest's own assertion - "a process with no children
             * has no reaped child time" - failed on the machine within a
             * minute of being written. It has M79's threads above it in
             * the same program, and every one that had been joined was
             * being reported as a child.
             *
             * So a joined thread's ticks land in the process's OWN
             * counters. What that makes RUSAGE_SELF is worth stating
             * plainly: the calling thread's time plus every thread of
             * this process that has already been joined. A running
             * sibling's time is not in it and cannot be without walking
             * the task table on every call, which is a cost this has no
             * measurement to justify - and POSIX's own wording for
             * RUSAGE_SELF ("the calling process") is satisfied by the
             * sum of what the process has actually finished doing. */
            parent->user_ticks += t->user_ticks;
            parent->sys_ticks += t->sys_ticks;
            parent->child_user_ticks += t->child_user_ticks;
            parent->child_sys_ticks += t->child_sys_ticks;
            /* M98: a joined thread shared this address space, so its
             * peak is this process's own peak and not a child's - the
             * same split the ticks above make, for the same reason. */
            if (t->max_rss_pages > parent->max_rss_pages) {
                parent->max_rss_pages = t->max_rss_pages;
            }
            if (t->child_max_rss_pages > parent->child_max_rss_pages) {
                parent->child_max_rss_pages = t->child_max_rss_pages;
            }
        } else {
            parent->child_user_ticks += t->user_ticks + t->child_user_ticks;
            parent->child_sys_ticks += t->sys_ticks + t->child_sys_ticks;
            /* M98: the largest a child or a child's child ever was.
             * A maximum, not a sum - see sched.h's field comment. */
            if (t->max_rss_pages > parent->child_max_rss_pages) {
                parent->child_max_rss_pages = t->max_rss_pages;
            }
            if (t->child_max_rss_pages > parent->child_max_rss_pages) {
                parent->child_max_rss_pages = t->child_max_rss_pages;
            }
        }
    }

    uint8_t *stack = t->stack_base;
    t->stack_base = NULL;
    t->kernel_stack_top = 0;
    t->generation++;
    t->state = TASK_FREE;
    t->pending_signal = 0;
    /* Q13: and the job-control stop, which this list did not clear.
     *
     * Found by the first host test that asked what a recycled slot
     * inherits. `pending_signal` was cleared here from the beginning and
     * `pending_stop` was not, so a slot whose previous occupant had been
     * sent a ^Z it never got round to taking handed that stop to the
     * next process to land in the row - which suspends itself at its
     * first checkpoint, before it has run a line, with no terminal
     * involved and nothing to resume it.
     *
     * It was close to unreachable before M85's second attempt, because
     * a pending stop was only ever taken at a timer tick that found the
     * task current, so most of them were never taken at all. Adding the
     * two checkpoints that make ^Z work is what made this one bite, and
     * this tier is what found it - reaching it on a booted machine
     * means killing a process in the window between a ^Z and the tick
     * that takes it. */
    t->pending_stop = 0;
    t->stopped_sig = 0;
    t->stop_reported = 0;
    t->reaped = 0;
    /* M88: and the CPU time goes with the occupant, not the slot. Zeroed
     * both here and at spawn on purpose: this is the release path, and a
     * free slot holding a dead task's totals is the same class of thing
     * as the caps field two lines down holding its authority. */
    t->user_ticks = 0;
    t->sys_ticks = 0;
    t->child_user_ticks = 0;
    t->child_sys_ticks = 0;
    t->max_rss_pages = 0;        /* M98 */
    t->child_max_rss_pages = 0;  /* M98 */
    t->exit_signal = 0; /* M84: not killed until something kills it */
    t->exit_code = 0;   /* M106: a free slot holds no verdict either - a stale one made M55's diagnostic report a segfault that never happened */
    t->prio = PRIO_INTERACTIVE; /* M106 - see the note at the spawn site */
    t->full_slices = 0;
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
    /* M99: and the SA_SIGINFO bits with them - this slot is about to be
     * reused, and nothing the task that just died installed belongs to
     * whatever runs here next. */
    t->sig_siginfo = 0;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_addr = 0;
    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = SIG_DFL_ADDR;
    }
    /* M78: the frames themselves went back with the address space in
     * task_exit_with_code; this is the bookkeeping that described them. */
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i].base = 0;
        t->mmaps[i].pages = 0;
    }
    t->fs_base = 0; /* M96 - see the note at the other reset site */
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
        pmm_free_contiguous((uint64_t)(uintptr_t)stack, TASK_STACK_SIZE / 4096);
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

/* ---- M82: demand paging ----------------------------------------------
 *
 * See sched.h for the contract. What is worth arguing here is the set of
 * things this deliberately refuses to fill, because every one of them is
 * a fault that used to be fatal and has to stay fatal:
 *
 *   - a fault on a page that is PRESENT. That is a protection violation,
 *     not a first touch: a write to a read-only mapping, or (once M83
 *     exists) a write to a shared copy-on-write page. Filling it would
 *     mean silently granting write access nobody asked for.
 *   - a fault outside the mmap arena. The heap, the stack, the image and
 *     the shm window are all mapped eagerly by whoever owns them, so a
 *     fault there is a wild pointer and always was.
 *   - a fault inside the arena but outside any mapping this process
 *     holds. Reserving address space is what SYS_mmap does; touching
 *     address space nobody reserved is exactly the bug the arena's
 *     bounds exist to catch.
 *   - a WRITE to a mapping the caller asked for read-only. The promise
 *     SYS_mmap made is in the region's `prot`, and it is the only place
 *     that promise is written down before a page exists.
 *
 * Out of physical memory returns 0 too, so a process that touches more
 * than the machine has dies rather than the machine panicking. That is a
 * real behaviour change from M78's eager mapping, where the same program
 * would have been told -1 by SYS_mmap and could have handled it. It is
 * the trade demand paging always makes and it is worth stating plainly:
 * the failure moves from the call that reserves to the instruction that
 * touches.
 */
/* The one page-building step, shared by the fault path and the prefault
 * path below so there is one set of rules about what may be built and
 * one place they are written down. Returns 1 if the page is now there. */
/* M91: what a page in this address space is allowed to become, or
 * FILL_REFUSE. Split out of fill_one_page because there are now two kinds
 * of address that may be filled - a reserved mmap region, and the stack
 * below what was mapped at spawn - and because the answer is no longer
 * just yes/no: it is the set of permissions the new page gets, which for
 * the first time includes whether code may be fetched from it. */
#define FILL_REFUSE ((uint64_t)-1)

static uint64_t fill_policy(task_t *self, uint64_t page, int for_write, int for_exec) {
    /* The stack. Writable, never executable, and bounded by
     * USER_STACK_LIMIT rather than by anything a program can influence -
     * proc.h's USER_STACK_MAX_BYTES is the whole rule. The caller has
     * already decided whether this fault is close enough to the stack
     * pointer to be a stack access at all; see sched_fault_fill. */
    if (page >= USER_STACK_LIMIT && page < USER_STACK_TOP) {
        if (for_exec) {
            return FILL_REFUSE;
        }
        return VMM_FLAG_USER | VMM_FLAG_WRITABLE;
    }

    if (page < USER_MMAP_BASE || page >= USER_MMAP_LIMIT) {
        return FILL_REFUSE;
    }
    const mmap_region_t *region = 0;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (self->mmaps[i].pages == 0) {
            break; /* the table is kept sorted and packed - see mmap_slot_cmp_insert */
        }
        uint64_t start = self->mmaps[i].base;
        uint64_t end = start + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        if (page >= start && page < end) {
            region = &self->mmaps[i];
            break;
        }
    }
    if (!region) {
        return FILL_REFUSE;
    }
    /* M91: PROT_NONE is now a mapping rather than a refused argument, and
     * this is the line that makes it mean something. A region with no
     * access bits is address space reserved so that nothing else lands
     * there and so that touching it dies - a guard page. Filling it would
     * make it a guard page that guards nothing, which is exactly what
     * M78's comment said it declined to ship. */
    if ((region->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) {
        return FILL_REFUSE;
    }
    if (for_write && !(region->prot & PROT_WRITE)) {
        return FILL_REFUSE;
    }
    /* An instruction fetch from a region that is not PROT_EXEC is refused
     * here rather than being allowed to build a page the CPU will refuse
     * to run one instruction later. Both end in the same death; this one
     * ends in it immediately and without allocating a frame first. */
    if (for_exec && !(region->prot & PROT_EXEC)) {
        return FILL_REFUSE;
    }
    uint64_t flags = VMM_FLAG_USER;
    if (region->prot & PROT_WRITE) {
        flags |= VMM_FLAG_WRITABLE;
    }
    if (region->prot & PROT_EXEC) {
        flags |= VMM_FLAG_EXEC;
    }
    return flags;
}

/* M91 (second attempt): the region a fault landed in, or NULL.
 *
 * fill_policy already finds this and returns only the flags, which was
 * everything a fault needed while every mapping was anonymous. A
 * file-backed one needs the region itself - which file, which page of
 * it, and whether the frame is shared - so the lookup is factored out
 * rather than done twice. */
static const mmap_region_t *mmap_region_for(task_t *self, uint64_t page) {
    if (page < USER_MMAP_BASE || page >= USER_MMAP_LIMIT) {
        return 0;
    }
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t start = self->mmaps[i].base;
        uint64_t end = start + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        if (page >= start && page < end) {
            return &self->mmaps[i];
        }
    }
    return 0;
}

/* ---- M91 (second attempt): giving shared file pages back --------------
 *
 * A shared mapping's frames belong to kernel/mm/filemap.c, not to this
 * address space, so every path that stops using them has to say so
 * rather than freeing them: unmap, fork (which drops both sides' entries
 * so each re-faults and takes its own reference) and exit.
 *
 * **Only pages that are actually mapped.** A reference is taken by the
 * fault that maps a page and by nothing else, so a region's untouched
 * pages hold none - and putting one back would underflow the count and
 * free a frame another process is reading. The page table is the record
 * of which pages were faulted, which is why this asks it rather than
 * assuming a region is fully backed.
 *
 * Returns how many references were dropped, which is what the self-test
 * counts.
 */
int sched_release_shared_range(task_t *t, uint64_t start, uint64_t end) {
    int dropped = 0;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
        }
        if (!t->mmaps[i].shared || t->mmaps[i].handle < 0) {
            continue;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        uint64_t from = start > rstart ? start : rstart;
        uint64_t to = end < rend ? end : rend;
        for (uint64_t p = from; p < to; p += PAGE_SIZE) {
            if (!vmm_user_range_ok(t->pml4_phys, p, 1, 0)) {
                continue; /* never faulted in - holds no reference */
            }
            vmm_unmap_page_in(t->pml4_phys, p);
            filemap_put(t->mmaps[i].handle,
                        t->mmaps[i].file_page +
                            (uint32_t)((p - rstart) / PAGE_SIZE));
            dropped++;
        }
    }
    return dropped;
}

static int fill_one_page_ex(task_t *self, uint64_t page, int for_write, int for_exec) {

    /* Never build a page that already exists.
     *
     * The fault path cannot reach here with a present page - the P bit in
     * the error code is checked first - but the prefault path can, and
     * will the moment a page can be present *and* not satisfy the access
     * being prefaulted. That is exactly what M83's copy-on-write
     * introduces: a page present and read-only inside a region whose prot
     * says writable. Mapping a fresh frame over it would strand the old
     * one - a leak with no owner, invisible to every free-frame assertion
     * in this kernel because the count would simply be short.
     *
     * Guarded here rather than in the caller because there is one rule -
     * this function builds pages that do not exist - and one place is
     * where a rule survives the next milestone. */
    if (vmm_user_range_ok(self->pml4_phys, page, 1, 0)) {
        return 0;
    }

    uint64_t flags = fill_policy(self, page, for_write, for_exec);
    if (flags == FILL_REFUSE) {
        return 0;
    }

    /* ---- M91 (second attempt): where the bytes come from --------------
     *
     * Three cases, and the difference between them is the whole of
     * file-backed and shared mapping:
     *
     *   anonymous          - a fresh zeroed frame, which is what every
     *                        mapping was until now.
     *   file, MAP_PRIVATE  - a fresh frame with the file's page read
     *                        into it. Writes stay here: nothing else
     *                        points at this frame, so "private" is true
     *                        by construction rather than by a
     *                        copy-on-write that would have to happen
     *                        later.
     *   file, MAP_SHARED   - the ONE frame kernel/mm/filemap.c holds for
     *                        that (file, page). Every mapper gets the
     *                        same one, which is what makes a shared
     *                        mapping shared.
     */
    const mmap_region_t *region = mmap_region_for(self, page);
    if (region && region->handle >= 0 && region->shared) {
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        uint64_t sphys = filemap_get(region->handle, index,
                                     (region->prot & PROT_WRITE) != 0);
        if (sphys == 0) {
            /* No frame, no slot, or the file would not read. All three
             * are "the machine could not do it" rather than "this
             * address is not yours", which is the distinction
             * FILL_NO_MEMORY exists to draw. */
            return FILL_NO_MEMORY;
        }
        if (vmm_try_map_page_in(self->pml4_phys, page, sphys, flags) != 0) {
            filemap_put(region->handle, index);
            return FILL_NO_MEMORY;
        }
        return 1;
    }

    uint64_t phys = pmm_try_alloc_frame();
    if (phys == 0) {
        /* M102: FILL_NO_MEMORY, not 0. The difference matters at the
         * fault handler: 0 means "this address is not yours", which is a
         * bug in the program and earns SIGSEGV, and this means "it is
         * yours and the machine has nothing left", which is not the
         * program's fault and must not be reported as though it were. */
        return FILL_NO_MEMORY;
    }
    /* Zeroed, for the reason M78 gave when it did this eagerly:
     * anonymous memory that handed a process the previous owner's bytes
     * would leak one program's data into another's. */
    k_memset((void *)phys, 0, PAGE_SIZE);
    /* M91 (second attempt): a private file mapping reads the file over
     * the zeros. Zeroed first and then read into, rather than read into
     * directly, because a mapping may run past the end of the file - and
     * POSIX says the tail of the last page reads as zeros, which is the
     * one thing a bare read would leave holding the previous owner's
     * bytes. A read that fails leaves the page zeroed rather than
     * failing the fault: the mapping is valid and the file is short or
     * unreadable, which is a page of zeros everywhere else too. */
    if (region && region->handle >= 0) {
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        (void)vfs_handle_read(region->handle, (void *)phys, PAGE_SIZE,
                              index * PAGE_SIZE);
    }
    /* M102: the page table this needs is a frame too, and on a full
     * machine it is the one that is missing. Returning 0 sends the fault
     * handler down the same path as "no frame for the page itself",
     * which kills the faulting process rather than the machine. */
    if (vmm_try_map_page_in(self->pml4_phys, page, phys, flags) != 0) {
        pmm_free_frame(phys);
        return FILL_NO_MEMORY;
    }
    return 1;
}

/* The pre-M91 shape, for the prefault path, which never faults on an
 * instruction fetch. */
static int fill_one_page(task_t *self, uint64_t page, int for_write) {
    return fill_one_page_ex(self, page, for_write, 0);
}

/* M91: how far below the stack pointer a fault may be and still be a
 * stack access.
 *
 * The stack region is 64 MiB of address space this process owns and
 * nothing else can be. A rule of "any fault inside it grows it" would be
 * one case more generous than it should be, in exactly the way M82's own
 * notes warn about: a wild pointer 30 MiB below the stack would quietly
 * become a valid page instead of killing the process.
 *
 * So the fault has to be near the stack pointer. SysV's red zone is 128
 * bytes and a compiler may touch that far below rsp legitimately; a
 * function that allocates a large local by subtracting from rsp and then
 * writes into the middle of it faults further down than that, before rsp
 * has moved. 64 KiB covers every prologue GCC emits without
 * -fstack-clash-protection, and is small enough that a pointer computed
 * from garbage lands outside it.
 */
#define STACK_GROW_SLACK 65536ULL

int sched_fault_fill(uint64_t addr, uint64_t error_code, uint64_t user_rsp) {
    task_t *self = sched_vm_owner(sched_current());
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return 0; /* a kernel thread has no arena to fault into */
    }
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);

    /* A fault in the stack window that is not near the stack pointer is
     * not a stack access, and is refused before anything else looks at
     * it. Checked here rather than in fill_policy because rsp is a fact
     * about this fault, not about the address space. */
    if (page >= USER_STACK_LIMIT && page < USER_STACK_TOP) {
        if (addr + STACK_GROW_SLACK < user_rsp) {
            return 0;
        }
    }

    /* Bit 0 of the error code is P and bit 1 is W/R. The two bits split
     * this into the only two faults this kernel knows how to answer, and
     * everything else falls through to fatal:
     *
     *   not present            -> a first touch of a reserved mapping (M82)
     *   present, and a write   -> a write to a page shared by a fork (M83)
     *
     * The second case is narrower than it looks: vmm_cow_break refuses
     * anything not marked PTE_COW, and only pages that were *writable*
     * before the fork carry that mark. A write to the program's own text,
     * or to a PROT_READ mapping, is present and not marked, and stays as
     * fatal as it has always been. */
    if (error_code & 1u) {
        if (error_code & 2u) {
            return vmm_cow_break(self->pml4_phys, page);
        }
        return 0;
    }
    /* Bit 4 is I/D: the fault was an instruction fetch. M91 is the first
     * milestone in which that can be refused rather than merely noted. */
    return fill_one_page_ex(self, page, (error_code & 2u) != 0, (error_code & 16u) != 0);
}

/* ---- M82: prefaulting a buffer the kernel is about to touch -----------
 *
 * This is the half of demand paging that is invisible until it bites.
 *
 * A syscall that writes into a caller's buffer - SYS_read, SYS_getcwd,
 * SYS_recv, SYS_getdents - first asks user_range_ok whether the range is
 * really the caller's, and that question is answered by walking the
 * caller's page tables. A page that has been *reserved* by SYS_mmap and
 * never touched has no page table entry, so the honest answer to "is
 * this mapped" is no - and every one of those syscalls would have
 * started returning -1 for a buffer that a program had every right to
 * pass. `char *buf = mmap(...); read(fd, buf, n);` is not an exotic
 * thing to write.
 *
 * So the range is built before it is checked. Deliberately built here
 * rather than left to fault inside the copy loop: a page fault taken in
 * ring 0 while the kernel holds a lock is a much harder thing to reason
 * about than a loop that runs before any lock is taken, and this way
 * every kernel access to user memory still touches only present pages.
 *
 * Fills what it can and reports nothing. It is not this function's job to
 * decide whether the range is acceptable - user_range_ok still asks the
 * page tables afterwards, and a range this could not build is one that
 * check will refuse exactly as it did before.
 */
void sched_prefault_range(uint64_t addr, uint64_t len, int for_write) {
    if (len == 0) {
        return;
    }
    task_t *self = sched_vm_owner(sched_current());
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return;
    }
    /* M91: the whole range asked about once, before anything is done per
     * page. The common case by a very large margin is a buffer that is
     * already entirely mapped - a local on the stack, a static, an mmap
     * region the program has been using - and answering that in one
     * locked walk rather than in one locked walk per page matters now
     * that the stack window is checked as well as the arena. Before this
     * milestone a stack buffer left this function on the bounds test
     * below and did no walking at all; it must not become more expensive
     * than it was for the case that was already free. */
    if (vmm_user_range_ok(self->pml4_phys, addr, len, for_write)) {
        return;
    }
    uint64_t first = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (addr + len - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (last < first) {
        return; /* wrapped - user_range_ok refuses it below */
    }
    /* Bounded before looping, so a bogus length cannot turn this into a
     * walk over the whole address space. Two windows rather than one
     * since M91: a syscall buffer can legitimately be a large local that
     * the program allocated by subtracting from rsp and has not yet
     * touched, which is stack the fault handler has not been asked to
     * build. `read(fd, buf, sizeof buf)` into a 1 MiB local is the
     * ordinary case, and refusing it would be a -1 for a buffer the
     * program has every right to pass.
     *
     * Deliberately no stack-pointer heuristic here, unlike the fault
     * path, and the reason is that there is nothing to apply it to: this
     * address came from a syscall argument rather than from a faulting
     * instruction. The cost is that a wild syscall pointer inside the
     * process's own 64 MiB stack window is built rather than refused,
     * where before it would have been an EFAULT. That is the same trade
     * every Unix makes for the same reason. */
    int in_arena = !(last < USER_MMAP_BASE || first >= USER_MMAP_LIMIT);
    int in_stack = !(last < USER_STACK_LIMIT || first >= USER_STACK_TOP);
    if (!in_arena && !in_stack) {
        return;
    }
    for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
        if (vmm_user_range_ok(self->pml4_phys, page, 1, for_write)) {
            continue; /* already there and already good enough for this access */
        }
        /* M83: a page shared by a fork is present and read-only, so the
         * check above says no for a write - and fill_one_page refuses it
         * too, because the page exists. Breaking the sharing here is what
         * keeps "the kernel writing into a caller's buffer" working for a
         * process that has just forked, and it has to happen before the
         * copy loop for the same reason the fill does: so that no kernel
         * access to user memory ever takes a fault. */
        if (for_write && vmm_cow_break(self->pml4_phys, page)) {
            continue;
        }
        fill_one_page(self, page, for_write);
    }
}

/* ---- M83: a task that resumes where its parent was --------------------
 *
 * Every other task in this kernel starts at an entry point. A forked
 * child does not: it has to resume in the middle of its parent's
 * `int 0x80`, with every register the parent had and rax replaced by
 * zero. That is the whole difference between this and task_spawn_common,
 * and it is why the two are separate functions rather than one with a
 * flag - almost every line that looks the same means something different.
 *
 * The child's kernel stack is built in two halves. At the very top sits a
 * copy of the parent's own trap frame, which fork_return_to_user
 * (isr_asm.asm) will pop and `iretq` through. Below it sits the same
 * fabricated context_switch frame every new task gets, except that it
 * returns into fork_child_trampoline rather than task_entry_trampoline -
 * so the first time the scheduler picks this task, it lands there, and
 * that hands control to the frame above.
 *
 * What is inherited and what is not follows POSIX rather than following
 * task_spawn_common:
 *
 *   - signal DISPOSITIONS are inherited, where a spawn resets them. A
 *     spawn loads a different program and a handler address belongs to
 *     the image that installed it; a fork keeps the same image running,
 *     so keeping the handlers is the only answer that makes sense.
 *   - the mmap table is inherited, where a spawn starts empty. The
 *     child's address space is a copy of the parent's, so every address
 *     in that table means the same thing in it.
 *   - the heap cursors are inherited for the same reason.
 *   - the environment is NOT copied here, exactly as task_spawn_common
 *     does not: a kmalloc under sched_lock would invert this kernel's one
 *     lock-ordering rule. The fork syscall copies it after this returns.
 *   - the FPU state IS copied, where a spawn starts clean. Same argument
 *     as the handlers: this is the same program mid-computation, and
 *     starting the child from a zeroed FPU would corrupt a float that was
 *     live across the call.
 */
task_t *task_fork(uint64_t child_pml4, const isr_regs_t *regs) {
    _Static_assert(TASK_STACK_SIZE % 4096 == 0, "a kernel stack must be a whole number of frames");
    /* M102: try_, so the check below is reachable. It was written
     * against pmm_alloc_contiguous, which panics rather than returning 0,
     * so it has been dead code since it was written - the same shape of
     * dead guard this milestone found in proc.c's argument frames. */
    uint8_t *stack_base = (uint8_t *)(uintptr_t)pmm_try_alloc_contiguous(TASK_STACK_SIZE / 4096);
    if (!stack_base) {
        return (task_t *)0;
    }

    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);

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
            pmm_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
            return NULL;
        }
        slot = task_count++;
    }

    task_t *t = &tasks[slot];
    task_t *parent = current_task[smp_current_cpu()];

    t->id = PID_MAKE(slot, t->generation);
    t->entry = NULL; /* a fork has no entry point - see this function's header */
    t->arg = NULL;
    t->state = TASK_READY;
    t->pml4_phys = child_pml4;
    t->stack_base = stack_base;
    t->kernel_stack_top = (uint64_t)(stack_base + TASK_STACK_SIZE);

    for (int i = 0; i < MAX_FDS; i++) {
        /* M84: FD_CLOEXEC is copied, not acted on. It means "close this
         * when a different program starts", and a fork starts no program
         * - the child is still running this one. It is the exec that
         * closes them, which is the whole reason the flag is per
         * descriptor rather than per call. */
        t->fds[i] = parent->fds[i];
        fd_retain(&t->fds[i]);
    }
    t->parent_id = parent->id;
    t->pgid = parent->pgid;
    t->sid = parent->sid; /* M85 - see task_spawn_common */
    t->pending_signal = 0;
    t->reaped = 0;
    t->exit_signal = 0; /* M84: not killed until something kills it */
    /* M88: a fork inherits the parent's descriptors and its address
     * space, and inherits none of its CPU time. The child has not run.
     * This is the one field where "copy the parent" would be actively
     * wrong rather than merely generous - `times()` in a child that
     * reported its parent's accumulated ticks is how a build system
     * concludes it has been running for a week. */
    t->user_ticks = 0;
    t->sys_ticks = 0;
    t->child_user_ticks = 0;
    t->child_sys_ticks = 0;
    t->max_rss_pages = 0;        /* M98 */
    t->child_max_rss_pages = 0;  /* M98 */

    for (int i = 0; i < PATH_MAX_LEN; i++) {
        t->cwd[i] = parent->cwd[i];
        if (!parent->cwd[i]) {
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

    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = parent->sig_handler[i];
    }
    t->sig_restorer = parent->sig_restorer;
    t->sig_blocked = parent->sig_blocked;
    /* Pending signals are NOT inherited. POSIX says so, and the reason is
     * good: a signal was sent to the parent, and a child that had not
     * existed when it was sent has no business acting on it. */
    t->sig_pending = 0;
    /* M99: the SA_SIGINFO bits ARE inherited, because the handlers they
     * describe are - a fork keeps the parent's address space and its
     * dispositions, so a handler that expected three arguments before
     * the fork expects three after it. The scratch fields are not: they
     * describe a signal raised on the parent. */
    t->sig_siginfo = parent->sig_siginfo;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_addr = 0;

    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i] = parent->mmaps[i];
    }

    /* A fork produces a process, never a thread, even when the caller was
     * one. POSIX is explicit that only the calling thread survives into
     * the child, and a child that believed it was a thread of its
     * parent's group would share bookkeeping with an address space it no
     * longer shares. */
    t->tgid = t->id;
    t->is_thread = 0;
    t->exiting = 0;
    t->caps = parent->caps;

    for (size_t i = 0; i < sizeof(t->fpu_state); i++) {
        t->fpu_state[i] = parent->fpu_state[i];
    }

    /* M85 (second attempt): the thread pointer, which was NOT copied
     * here and had to be.
     *
     * A fork duplicates the address space, so the TLS block the parent's
     * %fs points at exists in the child at the same address - and the
     * child was getting fs_base 0. Every `errno = ...` in this libc is a
     * store through %fs, so the first *failing* call a forked child made
     * was a page fault at address 0. A child that only ever succeeded
     * never touched it, which is why forktest, exectest and every ported
     * program that works has been passing over this for four milestones:
     * the bug is on the error path of a process that has forked, and
     * nothing had put one there.
     *
     * Found by M85's pty fixture, whose child calls ioctl(TIOCSCTTY) on
     * a terminal another session already owned and died decoding the -1.
     * The two reset sites this pairs with are exec (a new address space,
     * so the pointer is stale) and slot recycling (a new process
     * entirely); a fork is the one case where carrying it over is the
     * correct answer, and it was the one case that did not. */
    t->fs_base = parent->fs_base;

    t->heap_brk = parent->heap_brk;
    t->heap_mapped_end = parent->heap_mapped_end;
    t->shm_next_vaddr = parent->shm_next_vaddr;
    set_task_name(t, parent->name);

    /* The parent's trap frame, at the very top of the child's own kernel
     * stack, with rax zeroed - which is the entire user-visible
     * difference between the two sides of a fork. */
    isr_regs_t *child_frame =
        (isr_regs_t *)(t->kernel_stack_top - sizeof(isr_regs_t));
    *child_frame = *regs;
    child_frame->rax = 0;

    /* And below it, the same fabricated context_switch frame every task
     * gets - see task_spawn_common for what each slot is - returning into
     * the fork trampoline instead of the ordinary one. */
    uint64_t *sp = (uint64_t *)child_frame;
    *(--sp) = (uint64_t)fork_child_trampoline; /* popped by `ret` */
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

    /* ---- M85: SIGCONT, before anything else ---------------------------
     *
     * A stopped task is not running, so it cannot take a signal in the
     * ordinary way - which means the signal that resumes it has to act
     * on the task table rather than on the task. Handled first, and
     * unconditionally, because a process that has ignored SIGCONT still
     * has to be resumable: POSIX says the *continue* happens whatever the
     * disposition, and only the optional handler is subject to it.
     * Otherwise a program could make itself permanently unstoppable in
     * the other direction - suspended forever with no way back. */
    if (sig == SIGCONT) {
        t->pending_stop = 0;
        if (t->state == TASK_STOPPED) {
            sched_resume_stopped(t);
        }
        /* Fall through: a handler for SIGCONT still runs if one is
         * installed, which is how a shell learns it was resumed. */
    }

    if (!SIG_IS_CATCHABLE(sig)) {
        /* SIGKILL, SIGSEGV if anything ever sends one, and M85's SIGSTOP.
         * No handler, no mask, no argument. */
        if (sig == SIGSTOP) {
            t->pending_stop = sig;
            sched_wake_task(t);
            return;
        }
        t->pending_signal = sig;
        /* M85: a stopped task has to become runnable to die. Nothing else
         * will ever schedule it, so a SIGKILL to a suspended process
         * would otherwise be a kill that never happens - which is the one
         * thing SIGKILL is not allowed to be. */
        if (t->state == TASK_STOPPED) {
            sched_resume_stopped(t);
        }
        sched_wake_task(t);
        return;
    }
    uint64_t h = t->sig_handler[sig];
    if (h == SIG_IGN_ADDR) {
        return;
    }
    if (h == SIG_DFL_ADDR) {
        /* M85: the default action, from the one table both the kernel and
         * user space read (SIG_DEFAULT_ACTION in system_api's signal.h).
         * This used to be "death for everything except SIGCHLD", with a
         * note that a table of one row is a table nobody reads. Job
         * control made it four. */
        switch (SIG_DEFAULT_ACTION(sig)) {
        case SIG_DFL_IGNORE:
        case SIG_DFL_CONTINUE:
            return; /* the resume itself was done above */
        case SIG_DFL_STOP:
            t->pending_stop = sig;
            sched_wake_task(t);
            return;
        default:
            break;
        }
        t->pending_signal = sig;
        if (t->state == TASK_STOPPED) {
            sched_resume_stopped(t);
        }
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

/* M85: raise `sig` on every live member of a process group.
 *
 * Lives here rather than in the tty because it is a fact about the task
 * table, and because SYS_kill's negative-pid form wants the same walk.
 * No permission check: the callers that have one (SYS_kill) ask before
 * calling, and the caller that does not (a terminal raising SIGINT on
 * its own foreground job) has no user to check - the authority is the
 * terminal's, established when the shell handed it the group. */
void sched_raise_signal_group(int pgid, int sig) {
    if (pgid == 0) {
        return;
    }
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_FREE || t->state == TASK_TERMINATED) {
            continue;
        }
        if (t->pgid == pgid) {
            sched_raise_signal(t, sig);
        }
    }
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
    slot->cloexec = 0; /* M84: a slot that holds nothing holds no flag either */
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
