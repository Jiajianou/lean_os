#include "sched.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/io.h" /* irq_save_disable/irq_restore */
#include "arch/x86_64/smp.h"
#include "drivers/pit.h"
#include "fs/openfile.h"
#include "ipc/pipe.h"
#include "ipc/shm.h" /* shm_free_by_owner - see task_exit_with_code */
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "panic.h"
#include "proc/proc.h" /* M54 - process_destroy_address_space, called from task_exit_with_code */
#include "signal.h" /* system_api/include/signal.h - SIGKILL/SIGTERM */

#define TASK_STACK_SIZE (8 * 1024)
#define SCHED_QUANTUM_TICKS 5 /* 5 * 10 ms PIT ticks = 50 ms time slice */

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
    if (t->pending_signal == SIGKILL || t->pending_signal == SIGTERM) {
        deliver_pending_signal_and_exit(t);
    }
    if (++ticks_in_slice[cpu] < SCHED_QUANTUM_TICKS) {
        return;
    }
    ticks_in_slice[cpu] = 0;
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
    set_task_name(t, "cpu-idle");
    current_task[cpu_id] = t;
    loaded_pml4_phys[cpu_id] = t->pml4_phys;
    task_count++;
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

static task_t *task_spawn_common(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                                  uint64_t heap_start, uint64_t shm_base) {
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
    return task_spawn_common(name, vmm_kernel_pml4_phys(), entry, arg, 0, 0);
}

task_t *task_spawn_in(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                       uint64_t heap_start, uint64_t shm_base) {
    return task_spawn_common(name, pml4_phys, entry, arg, heap_start, shm_base);
}

/* Round-robin: scan forward from `from`, wrapping, for the next READY
 * task. TASK_RUNNING (i.e. `from` itself, if nothing else is ready) and
 * TASK_TERMINATED slots are skipped. Caller must hold sched_lock - two
 * CPUs scanning/claiming concurrently without it could both pick the same
 * READY task. */
static task_t *pick_next(task_t *from) {
    /* M54: from->id is a pid now, not an index - PID_SLOT is where in the
     * table it actually lives. A free slot is skipped for the same reason
     * a terminated one is: it holds no runnable task. */
    int start = PID_SLOT(from->id);
    for (int offset = 1; offset <= task_count; offset++) {
        int i = (start + offset) % task_count;
        if (tasks[i].state == TASK_READY) {
            return &tasks[i];
        }
    }
    return from; /* nothing else ready - keep running this one */
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
    if (t->pending_signal == SIGKILL || t->pending_signal == SIGTERM) {
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
    if (t->pml4_phys != vmm_kernel_pml4_phys()) {
        int cpu = smp_current_cpu();
        uint64_t dead = t->pml4_phys;
        t->pml4_phys = vmm_kernel_pml4_phys();
        vmm_switch_address_space(t->pml4_phys);
        loaded_pml4_phys[cpu] = t->pml4_phys;
        process_destroy_address_space(dead);
    }

    t->exit_code = code;
    t->state = TASK_TERMINATED;
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
    sched_reset_fds_to_std(t);
    t->fds[0].type = FD_NONE; /* a free slot holds nothing at all, not even stdin/stdout */
    t->fds[1].type = FD_NONE;
    set_task_name(t, "");
    spin_unlock(&sched_lock);
    irq_restore(flags);
    if (stack) {
        kfree(stack);
    }
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
