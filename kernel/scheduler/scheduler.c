#include "scheduler.h"

#include "device/pty.h"
#include "device/tty.h"

#include <stddef.h>
#include <stdint.h>

#include "architecture/x86_64/cpu.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/global_descriptor_table.h"
#include "architecture/x86_64/io.h"
#include "architecture/x86_64/lapic.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "file_system/open_file.h"
#include "network/socket.h"
#include "inter_process_communication/pipe.h"
#include "file_system/flock.h"
#include "inter_process_communication/shared_memory.h"
#include "inter_process_communication/unix_socket.h"
#include "inter_process_communication/eventfd.h"
#include "inter_process_communication/timerfd.h"
#include "inter_process_communication/epoll.h"
#include "inter_process_communication/memfd.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "panic.h"
#include "process/process.h"
#include "library/kernel_library.h"
#include "mman.h"
#include "file_system/virtual_file_system.h"
#include "memory_management/file_mapping.h"
#include "signal.h"
#include "syscall.h"

static void scheduler_deliver_pending_signal(void);

#define TASK_STACK_SIZE (32 * 1024)
#define SCHEDULER_QUANTUM_TICKS 2

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);

static task_t *tasks;
static int task_count;

static task_t *current_task[MAX_CPUS];
static uint32_t ticks_in_slice[MAX_CPUS];
static volatile uint64_t switch_count[MAX_CPUS];
static uint32_t aging_ticks;

static volatile int need_resched[MAX_CPUS];

static int peak_live_tasks;
static uint64_t loaded_pml4_phys[MAX_CPUS];

static spinlock_t scheduler_lock;

/* Which processor this is - and so which task this is - is only true while
   nothing can move the caller to another one. With interrupts on, a tick
   between reading the index and using it can preempt the task and resume it
   on a different processor, still holding the old index.

   That was the four-core bug M172 to M182 chased. schedule() read the index
   and THEN disabled interrupts, so a task preempted in that gap went on, on
   its new processor, to treat the old processor's running task as its own:
   it saved its stack pointer into that task, installed the next task as the
   old processor's current one, and pointed the old processor's tss.rsp0 at
   the next task's stack. The old processor's next entry from ring 3 then
   landed one interrupt frame below a stack top that belonged to somebody
   else - the exact 0x1D0 M182 measured - and task_exit's copy of the same
   pattern wrote another processor's record of which page table it had
   loaded, so that processor could skip a CR3 switch and run a task in the
   wrong address space. One processor cannot migrate anything, which is why
   one processor never saw it. */
static task_t *current_task_now(void) {
    uint64_t flags = irq_save_disable();
    task_t *t = current_task[smp_current_cpu()];
    irq_restore(flags);
    return t;
}

static uint64_t event_sequence;

static int blocked_count;

static task_t *pick_next(task_t *from, int cpu);
static void kick_idle_cpus(int wanted);
void scheduler_dump_cpus(void);
static int wake_expired_until(uint64_t now_ms, uint64_t *next_deadline_ms);
static void arm_deadline_timer(int cpu, uint64_t deadline_ms);
static volatile uint64_t deadline_timer_interrupts;
static volatile uint64_t lost_deadline_arms;
static uint64_t armed_deadline_ms[MAX_CPUS];
static void fire_expired_alarms(uint64_t now_ms);
static int raise_signal_locked(task_t *t, int sig);
static void raise_child_signal(int parent_id, int child_pid, int32_t status);
static void unblock_self(task_t *self);
static void block_on(const void *chan, uint64_t space, uint64_t deadline_ms, spinlock_t *lock,
                     uint64_t *flags, int interruptible);


const int scheduler_poll_channel = 0;
const int scheduler_keyboard_channel = 0;
const int scheduler_input_object = 0;

static uint64_t idle_ticks[MAX_CPUS];
static uint64_t total_ticks[MAX_CPUS];

/* M197. The busy figure counted a tick as busy when ready work existed
   ANYWHERE while this core idled - on any tick where a timed wait expired
   every idle core reported busy - so an idle desktop read 30% on eight
   cores. What a core did is the time it spent halted, measured with the TSC:
   an interval opens just before hlt and closes when hlt returns or when the
   scheduler switches away on that core, whichever happens first. */
static uint64_t halted_cycles[MAX_CPUS];
static uint64_t halt_since[MAX_CPUS];

#ifdef LEANOS_HOST_TEST
static uint64_t tsc_read(void) {
    return 0;
}

static uint64_t tsc_to_us(uint64_t cycles) {
    return cycles;
}

#define HALT_WITH_INTERRUPTS() ((void)0)
#define CPU_RELAX() ((void)0)
#else
#include "architecture/x86_64/timestamp_counter.h"

#define HALT_WITH_INTERRUPTS() __asm__ volatile("sti; hlt; cli" ::: "memory")
#define CPU_RELAX() __asm__ volatile("pause")
#endif

/* M198: bumped when a task exits and when one is reaped - the two moments
   the answer to "is that program still there" changes - so a task that
   keeps windows for other programs can ask whether anything changed since it
   last looked instead of polling each one. */
static uint64_t exit_sequence;

uint64_t scheduler_exit_sequence(void) {
    return __atomic_load_n(&exit_sequence, __ATOMIC_ACQUIRE);
}

static void close_halt_interval(int cpu) {
    if (halt_since[cpu] != 0) {
        halted_cycles[cpu] += tsc_read() - halt_since[cpu];
        halt_since[cpu] = 0;
    }
}

void scheduler_halt(void) {
    uint64_t flags = irq_save_disable();
    int cpu = smp_current_cpu();
    halt_since[cpu] = tsc_read();
    HALT_WITH_INTERRUPTS();
    close_halt_interval(smp_current_cpu());
    irq_restore(flags);
}

uint64_t scheduler_halted_us(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? tsc_to_us(halted_cycles[cpu]) : 0;
}

void scheduler_debug_dump(const char *label) {
    kernel_log_puts("[sched-dump] ");
    kernel_log_puts(label);
    kernel_log_putc('\n');
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_FREE) {
            continue;
        }
        kernel_log_puts("  slot=");
        kernel_log_put_dec((uint32_t)i);
        kernel_log_puts(" pid=");
        kernel_log_put_dec((uint32_t)t->id);
        kernel_log_puts(" name=");
        kernel_log_puts(t->name);
        kernel_log_puts(" state=");
        kernel_log_put_dec((uint32_t)t->state);
        kernel_log_puts(" chan=0x");
        kernel_log_put_hex32((uint32_t)(uint64_t)t->wait_chan);
        kernel_log_puts(" deadline=");
        kernel_log_put_dec((uint32_t)t->wake_deadline_ms);
        kernel_log_puts(" ticks=");
        kernel_log_put_dec((uint32_t)t->user_ticks);
        kernel_log_puts("u/");
        kernel_log_put_dec((uint32_t)t->sys_ticks);
        kernel_log_puts("s tgid=");
        kernel_log_put_dec((uint32_t)t->tgid);
        if (t->kernel_stack_top != 0 && t->pml4_phys != virtual_memory_kernel_pml4_phys()) {
            const isr_regs_t *frame =
                (const isr_regs_t *)(t->kernel_stack_top - sizeof(isr_regs_t));
            kernel_log_puts(" user-rip=0x");
            kernel_log_put_hex64(frame->rip);
        }
        kernel_log_putc('\n');
    }
}

void scheduler_idle_enter(void) {
    uint64_t flags = irq_save_disable();
    int cpu = smp_current_cpu();
    task_t *self = current_task[cpu];
    if (self && self->idle_wait_depth < 255) {
        self->idle_wait_depth++;
    }
    irq_restore(flags);
}

int scheduler_task_is_idle_waiting(const task_t *t) {
    return t && t->idle_wait_depth > 0;
}

void scheduler_idle_exit(void) {
    uint64_t flags = irq_save_disable();
    int cpu = smp_current_cpu();
    task_t *self = current_task[cpu];
    if (self && self->idle_wait_depth > 0) {
        self->idle_wait_depth--;
    }
    irq_restore(flags);
}

static void idle_task_body(void *arg) {
    (void)arg;
    cpu_enable_interrupts();
    for (;;) {
        scheduler_halt();
    }
}

/* M178: one for every CPU that could ever come online, and the caller does not
   get to say how many - a count that has to match the machine is a count
   somebody will get wrong, and somebody did: this was 2 from before there was
   an SMP bringup at all, and smp_init runs five hundred lines after the call.
   The number has to cover every CPU because a task that has marked itself
   TERMINATED calls schedule() and must not come back, and the only way out of
   pick_next for a terminated task is an idle one. Each AP can fall back to its
   own idle identity; the BOOT cpu has none, so these are all it has. */
void scheduler_spawn_idle_tasks(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        task_t *t = task_spawn("idle", idle_task_body, (void *)0);
        if (!t) {
            panic("sched: could not spawn an idle task");
        }
        t->is_idle = 1;
        t->parent_id = -1;
        t->lineage_id = -1;
    }
}

void scheduler_mark_self_idle(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    current_task[smp_current_cpu()]->is_idle = 1;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
}

uint64_t scheduler_idle_ticks(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? idle_ticks[cpu] : 0;
}

uint64_t scheduler_total_ticks(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? total_ticks[cpu] : 0;
}

void scheduler_account_tick(int user) {
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

static void set_task_name(task_t *t, const char *name);

void scheduler_set_task_name(task_t *t, const char *name) {
    set_task_name(t, name);
}

static void copy_task_name(char *out, const char *name) {
    int i = 0;
    if (name) {
        for (; name[i] && i < TASK_NAME_MAX - 1; i++) {
            out[i] = name[i];
        }
    }
    out[i] = '\0';
}

static void set_task_name(task_t *t, const char *name) {
    copy_task_name(t->name, name);
    copy_task_name(t->program, name);
}

void scheduler_set_thread_name(task_t *t, const char *name) {
    copy_task_name(t->name, name);
}

/* A task's first frame carries rflags with IF CLEAR, and interrupts are
   enabled here, after the lock is dropped. schedule() holds scheduler_lock
   across context_switch, and context_switch's popfq restores whatever flags
   the incoming frame has: a task that had been running saved its flags
   inside schedule() with interrupts off, but a NEW task's frame is built by
   hand, and for a hundred and seventy milestones it said 0x202. That is IF
   set, two instructions before the unlock below - and a timer tick that came
   due while schedule() had interrupts off fires at that popfq, takes the
   tick path that wants scheduler_lock, and spins on it for ever on the one
   CPU that could release it. It was "about one boot in ten hangs" for years;
   M170's blocking sleep made the tick take the lock on most ticks of an idle
   machine, and it became three boots in three (M171). */
static void task_entry_trampoline(void) {
    task_t *t = current_task[smp_current_cpu()];
    spin_unlock(&scheduler_lock);
    cpu_enable_interrupts();
    t->entry(t->arg);
    task_exit();
}

extern void fork_return_to_user(void *frame) __attribute__((noreturn));

static void fork_child_trampoline(void) __attribute__((noreturn));
static void fork_child_trampoline(void) {
    spin_unlock(&scheduler_lock);
    /* Interrupts stay off from here to the iretq, which restores the
       parent's user-mode flags - IF set - with the lock long released. */
    task_t *t = current_task[smp_current_cpu()];
    fork_return_to_user((void *)(t->kernel_stack_top - sizeof(isr_regs_t)));
}

static int resume_stopped_locked(task_t *t);

void scheduler_resume_stopped(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    (void)resume_stopped_locked(t);
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
}

static void take_pending_stop(task_t *t) {
    int sig = t->pending_stop;
    t->pending_stop = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    t->state = TASK_STOPPED;
    t->stopped_sig = sig;
    t->stop_reported = 0;
    event_sequence++;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    scheduler_wake_all((const void *)t);
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    schedule();
}

void scheduler_take_pending_stop_if_any(task_t *t) {
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

void scheduler_tick_cpu(int cpu) {
    task_t *t = current_task[cpu];

    total_ticks[cpu]++;
    int work_waiting = 0;
    if (t->is_idle || scheduler_task_is_idle_waiting(t)) {
        uint64_t f = irq_save_disable();
        spin_lock(&scheduler_lock);
        int nothing_else = 1;
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].state == TASK_READY && !tasks[i].is_idle) {
                nothing_else = 0;
                break;
            }
        }
        spin_unlock(&scheduler_lock);
        irq_restore(f);
        if (nothing_else) {
            idle_ticks[cpu]++;
        } else {
            work_waiting = 1;
        }
    }

    /* M199: a sleeper this tick wakes is work for this core too. The check
       above ran first, so an idle core that woke a task here went back to
       halting beside it until the NEXT tick - a 3 ms sleep took 15 on a
       quiet machine. Other idle cores are kicked; this one looks. */
    {
        uint64_t next_deadline;
        if (wake_expired_until(clock_monotonic_ms(), &next_deadline) > 0 && t->is_idle) {
            work_waiting = 1;
        }
    }
    fire_expired_alarms(clock_monotonic_ms());
    /* A deadline this core armed for and never heard about - the tick is
       already a period past it - is forgotten, or every later, later deadline
       would be taken as covered by it and the timer would never be armed
       again. */
    {
        uint64_t armed = armed_deadline_ms[cpu];
        if (armed != 0 && clock_monotonic_ms() > armed + 2u * (1000u / PIT_HZ)) {
            armed_deadline_ms[cpu] = 0;
            lost_deadline_arms++;
        }
    }

    /* An idle core with work ready does not wait out the rest of its idle
       quantum for it. */
    if (t->is_idle && work_waiting) {
        ticks_in_slice[cpu] = 0;
        schedule();
        return;
    }

    if (t->sleep_locks_held == 0 && !t->ending) {
        if (t->pending_signal != 0) {
            deliver_pending_signal_and_exit(t);
        }
        if (t->pending_stop != 0) {
            take_pending_stop(t);
        }
    }
    if (cpu == 0 && ++aging_ticks >= SCHEDULER_AGING_TICKS) {
        aging_ticks = 0;
        uint64_t af = irq_save_disable();
        spin_lock(&scheduler_lock);
        for (int i = 0; i < task_count; i++) {
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].last_block_tick = pit_get_ticks();
            need_resched[smp_current_cpu()] = 1;
        }
        spin_unlock(&scheduler_lock);
        irq_restore(af);
    }

    int preempt = need_resched[cpu] && t->prio == PRIO_BATCH;
    need_resched[cpu] = 0;
    if (!preempt && ++ticks_in_slice[cpu] < SCHEDULER_QUANTUM_TICKS) {
        return;
    }
    ticks_in_slice[cpu] = 0;

    /* M198. A task holding a sleep lock is not taken off the processor by
       the timer. It runs with interrupts on - answering IPIs and shootdowns,
       unlike the spinlock it replaced - but preempting it hands the processor
       to tasks that can only find the lock taken and go back to sleep, and
       four tasks writing files at once went from 0.32 s to 1.94 s that way
       (four_writers_us). It still gives the processor up whenever it blocks,
       and the first tick after it lets go takes it off. */
    if (t->sleep_locks_held > 0) {
        ticks_in_slice[cpu] = SCHEDULER_QUANTUM_TICKS - 1;
        return;
    }

    if (t->full_slices < 255) {
        t->full_slices++;
    }
    if (t->full_slices >= SCHEDULER_BATCH_THRESHOLD &&
        pit_get_ticks() - t->last_block_tick > SCHEDULER_AGING_TICKS) {
        t->prio = PRIO_BATCH;
    }
    schedule();
}

static void scheduler_tick(void) {
    scheduler_tick_cpu(smp_current_cpu());
    smp_broadcast_schedule_tick();
}

void scheduler_init(void) {
    /* Page-aligned, because a task carries its FPU state and that is aligned;
       zeroed, because every field of a free slot is expected to start at
       zero the way the static table's did. */
    uint64_t table_pages = (sizeof(task_t) * MAX_TASKS + 4095) / 4096;
    tasks = (task_t *)(uintptr_t)physical_memory_try_alloc_contiguous_anywhere(table_pages);
    if (!tasks) {
        panic("scheduler: no memory for the task table");
    }
    k_memset(tasks, 0, table_pages * 4096);
    /* M175: a free slot must not look bound to cpu 0. Every field starts at
       zero, and zero is a real CPU number. */
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].home_cpu = -1;
    }
    tasks[0].state = TASK_RUNNING;
    tasks[0].generation = 0;
    tasks[0].id = PID_MAKE(0, 0);
    tasks[0].stack_base = NULL;
    tasks[0].kernel_stack_top = 0;
    tasks[0].pml4_phys = virtual_memory_kernel_pml4_phys();
    fpu_state_init(tasks[0].fpu_state);
    /* The first task's table, which every other one is descended from. */
    tasks[0].descriptor_table = descriptor_table_new();
    if (!tasks[0].descriptor_table) {
        panic("scheduler_init: no memory for the first descriptor table");
    }
    tasks[0].descriptor_table->slots[0].type = FILE_DESCRIPTOR_STDIN;
    tasks[0].descriptor_table->slots[1].type = FILE_DESCRIPTOR_STDOUT;
    tasks[0].descriptor_table->slots[2].type = FILE_DESCRIPTOR_STDOUT;
    tasks[0].parent_id = -1;
    tasks[0].lineage_id = -1;
    tasks[0].pgid = 0;
    tasks[0].sid = 0;
    tasks[0].caps = CAP_ALL;
    tasks[0].kernel_task = 1;
    tasks[0].tgid = tasks[0].id;
    tasks[0].cwd[0] = '/';
    tasks[0].cwd[1] = '\0';
    tasks[0].env_block = NULL;
    tasks[0].cmdline_block = NULL;
    tasks[0].cmdline_length = 0;
    tasks[0].env_length = 0;
    tasks[0].env_count = 0;
    set_task_name(&tasks[0], "kernel");
    tasks[0].home_cpu = 0;
    task_count = 1;
    current_task[0] = &tasks[0];
    loaded_pml4_phys[0] = tasks[0].pml4_phys;

    pit_set_tick_hook(scheduler_tick);
}

void scheduler_init_ap(int cpu_id) {
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
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
    t->id = PID_MAKE(slot, t->generation);
    t->stack_base = NULL;
    t->kernel_stack_top = 0;
    t->pml4_phys = virtual_memory_kernel_pml4_phys();
    fpu_state_init(t->fpu_state);
    /* Each processor's idle identity is its own process and gets its own
       table, so nothing it does can reach another CPU's. */
    if (!t->descriptor_table) {
        t->descriptor_table = descriptor_table_new();
    }
    if (!t->descriptor_table) {
        panic("sched_init_ap: no memory for a CPU's descriptor table");
    }
    t->descriptor_table->slots[0].type = FILE_DESCRIPTOR_STDIN;
    t->descriptor_table->slots[1].type = FILE_DESCRIPTOR_STDOUT;
    t->descriptor_table->slots[2].type = FILE_DESCRIPTOR_STDOUT;
    t->parent_id = -1;
    t->lineage_id = -1;
    t->pgid = 0;
    t->sid = 0;
    t->pending_signal = 0;
    t->reaped = 0;
    t->exit_signal = 0;
    t->caps = CAP_ALL;
    t->is_idle = 1;
    t->kernel_task = 1;
    t->tgid = t->id;
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    t->env_block = NULL;
    t->cmdline_block = NULL;
    t->cmdline_length = 0;
    t->env_length = 0;
    t->env_count = 0;
    set_task_name(t, "cpu-idle");
    t->home_cpu = cpu_id;
    current_task[cpu_id] = t;
    loaded_pml4_phys[cpu_id] = t->pml4_phys;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
}

static int thread_group_has_live_members(int group, const task_t *except);
static int group_has_live_members_locked(int group, const task_t *except);

/* What task_exit learns, under the lock, when the task ending is the last of
   its process. */
typedef struct {
    int happened;
    task_t *leader;
    int pid;
    int parent_id;
    int32_t status;
} process_end_t;

/* Reset the slots of leaders whose groups have finished since they were
   last looked at - and, since M205, of threads nobody is ever going to join.

   scheduler_reap_slot holds a leader's slot while its threads are still
   running, and releases it when the last of them is reaped. A thread nobody
   joins is reaped by nobody, so without this the leader's slot would be held
   until the machine stopped - and a task table with 128 entries in it fills
   up quietly, as spawns that fail rather than as anything that says why.

   Here because it has to run somewhere that can take a lock and wait, and a
   spawn is the moment the answer matters. task_exit cannot do it: a fatal
   signal is delivered on the timer interrupt, and scheduler_reap_slot waits
   for a task to leave its kernel stack.

   A terminated thread is only ever reaped by a pthread_join, so two kinds
   were never reaped at all: a detached one, and every thread of a process
   that has died - nobody is left to join them. Chromium's thread pools make
   their workers detached and retire them when idle, and a browser that
   exits leaves a hundred and fifty threads behind it. Both stayed in the
   table until the machine stopped; the laptop's second Browser launch found
   it full, and its renderers died on "pthread_create: EAGAIN". */
static int nobody_will_join(const task_t *thread) {
    return thread->detached || !thread_group_has_live_members(thread->tgid, (const task_t *)0);
}

/* M224: a process that has ended is a ZOMBIE until its parent has waited for
   it, and only then is its slot anybody's. M168 wrote this sweep to release
   leaders whose wait() had been held back by live threads, and it released
   every terminated leader instead - so the next spawn, by anybody, destroyed
   the exit status of every child whose parent had not collected it yet.
   libuv asks waitpid(pid, WNOHANG) of each child it started, got "no such
   child", and never reported the exit: Node spawning forty `ls` and then
   forking once heard from none of the forty, ever.

   Released without a wait only when nobody is left who could wait - the
   parent process has ended, or the parent is the kernel, whose self-tests
   reap by slot and have always relied on this sweep to do it. */
static int nobody_will_wait(const task_t *leader) {
    if (leader->reaped || leader->parent_id <= 0) {
        return 1;
    }
    return !thread_group_has_live_members(leader->parent_id, (const task_t *)0);
}

void scheduler_release_finished_tasks(void) {
    for (int i = 0; i < task_count; i++) {
        task_t *o = &tasks[i];
        if (o->state != TASK_TERMINATED || !o->is_thread || !nobody_will_join(o)) {
            continue;
        }
        scheduler_reap_slot(o);
    }
    for (int i = 0; i < task_count; i++) {
        task_t *o = &tasks[i];
        if (o->state != TASK_TERMINATED || o->is_thread) {
            continue;
        }
        if (thread_group_has_live_members(o->tgid, o) || !nobody_will_wait(o)) {
            continue;
        }
        scheduler_reap_slot(o);
    }
}

int scheduler_detach_thread(task_t *caller, int thread_id) {
    task_t *t = scheduler_task_by_id(thread_id);
    if (!t || !caller || !t->is_thread || t->tgid != caller->tgid) {
        return -1;
    }
    t->detached = 1;
    return 0;
}

/* M225 (process-lifetimes): who may give a process another thread. The new
   thread takes a reference on the process's descriptor table, and
   descriptor_table_reference panics on a table at zero - "a table at zero is
   never revived" - which was true only because the one caller passed
   scheduler_current(). Any other leader could be a process that is exiting
   on another processor, whose last task has taken its table away (cleared
   under this lock, released after it) and whose count is about to reach, or
   has reached, zero. So the rule is now checked rather than relied on: the
   CALLER must be running in that process and still hold the table - it is
   the leader itself, or a thread of it - so the count is at least the
   caller's own one for as long as the caller runs, which is longer than
   this. Checked under scheduler_lock and before anything is counted. */
static int may_add_a_thread_to_locked(const task_t *caller, const task_t *thread_of) {
    return caller && thread_of && caller->descriptor_table &&
           caller->tgid == thread_of->tgid &&
           caller->descriptor_table == thread_of->descriptor_table &&
           !caller->ending;
}

static task_t *task_spawn_common(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                                  uint64_t heap_start, uint64_t shared_memory_base, task_t *thread_of,
                                  const task_spawn_setup_t *setup) {
    _Static_assert(TASK_STACK_SIZE % 4096 == 0, "a kernel stack must be a whole number of frames");
    scheduler_release_finished_tasks();
    uint8_t *stack_base = (uint8_t *)(uintptr_t)physical_memory_try_alloc_contiguous_anywhere(TASK_STACK_SIZE / 4096);
    if (!stack_base) {
        return (task_t *)0;
    }

    /* The table is allocated out here for the same reason the kernel stack
       is: inside the locked region there is nowhere to fail to. A process
       needs one, a thread shares its creator's and asks for nothing. */
    file_descriptor_table_t *fresh = (file_descriptor_table_t *)0;
    if (!thread_of) {
        fresh = descriptor_table_new();
        if (!fresh) {
            physical_memory_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
            return (task_t *)0;
        }
        /* M225: and filled out here, before scheduler_lock, as task_fork's
           has been since M204: a reference taken on a pipe end under
           scheduler_lock is pipe_lock inside scheduler_lock, which a pipe
           writer takes the other way round. The copy is made under the
           caller's table lock, so a sibling thread closing one of these
           descriptors at this moment either closes it before the copy (and
           the child does not get it) or after the child's reference is
           taken - never between.

           A program the KERNEL starts (M224) gets the three standard
           descriptors and nothing else. The boot self-tests open pipes on
           the kernel task's own table, and every one left open - two before
           that milestone, and a third that libctest's check then found -
           went to every program spawned afterwards as a descriptor 3 nobody
           had given it. Auditing each test found them one at a time; this
           ends the class. A test that captures a program's output still
           points 1 and 2 at its pipe first, which is inheritance this keeps;
           and a kernel THREAD, which shares the kernel's address space,
           takes the other branch and keeps the table it is a part of.
           Close-on-exec descriptors stay behind too: a spawn replaces the
           image, which is what the flag is about. */
        task_t *spawner = scheduler_current();
        const int from_kernel = spawner->pml4_phys == virtual_memory_kernel_pml4_phys() &&
                                pml4_phys != virtual_memory_kernel_pml4_phys();
        descriptor_table_copy(fresh, spawner->descriptor_table, 1,
                              from_kernel ? 3 : MAX_FILE_DESCRIPTORS);
    }

    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    task_t *caller = current_task[smp_current_cpu()];
    if (thread_of && !may_add_a_thread_to_locked(caller, thread_of)) {
        spin_unlock(&scheduler_lock);
        irq_restore(flags);
        physical_memory_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
        kernel_log_puts("[sched] refused a thread for a process the caller is not running in\n");
        return (task_t *)0;
    }
    int slot = -1;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (task_count >= MAX_TASKS) {
            spin_unlock(&scheduler_lock);
            irq_restore(flags);
            physical_memory_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
            descriptor_table_release(fresh);
            return NULL;
        }
        slot = task_count++;
    }
    task_t *t = &tasks[slot];
    t->id = PID_MAKE(slot, t->generation);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->pml4_phys = pml4_phys;
    t->stack_base = stack_base;
    t->kernel_stack_top = (uint64_t)(stack_base + TASK_STACK_SIZE);
    t->home_cpu = -1;
    t->kernel_activity = KERNEL_ACTIVITY_NONE;
    t->syscalls = 0;
    t->descriptor_uses = (file_descriptor_use_t *)0;
    t->stamp_syscalls_seen = 0;

    if (thread_of) {
        /* A thread. It does not get a copy of anything: it points at the
           process's one table, so a descriptor either thread opens is open
           for both and one either closes is closed for both. */
        t->descriptor_table = thread_of->descriptor_table;
        descriptor_table_reference(t->descriptor_table);
    } else {
        /* Filled out above, before the lock. */
        t->descriptor_table = fresh;
    }
    /* M224: the parent is a PROCESS. It was the calling thread's own id, so a
       child spawned on one thread could not be waited for from another -
       waitpid answered "no such child" - which is not POSIX's rule and is
       exactly how libuv works: the loop thread reaps what any thread spawned. */
    t->parent_id = caller->tgid;
    t->lineage_id = caller->tgid;
    t->kernel_task = pml4_phys == virtual_memory_kernel_pml4_phys();
    /* M225: a program the KERNEL starts leads a session and a process group
       of its own, as init does on every Unix (1, 1, 1 in ps). It used to
       inherit the kernel's "group" 0, which made every program on the
       machine - compositor, shells, test runners - one group with the
       kernel, its idle tasks, journal and tcp-timer: kill(0) from any of
       them was kill(everything), and POSIX has no process group 0 for
       getpgrp() to answer or for kill(0) to mean. Its own children inherit
       its group as before, so `kill 0` from a shell the kernel started ends
       that shell's own family and nothing else. A kernel thread stays in
       group 0 with the kernel; nothing a user process sends reaches it.
       Group leaders cannot setsid(): a program that wants a session of its
       own forks first, which is what login_tty, libuv's detached spawn and
       every daemon already do - and what they must do under a shell on
       Linux, where every job is a group leader.

       And a kernel thread is in the kernel's group whoever asked for it:
       wifi-dhcp is started inside a system call a program made, and
       inheriting that program's group put a kernel thread in a group a
       user process could signal. A thread is in its own process's group. */
    if (thread_of) {
        t->pgid = thread_of->pgid;
        t->sid = thread_of->sid;
    } else if (t->kernel_task) {
        t->pgid = 0;
        t->sid = 0;
    } else if (scheduler_task_is_kernel(caller)) {
        t->pgid = t->id;
        t->sid = t->id;
    } else {
        t->pgid = caller->pgid;
        t->sid = caller->sid;
    }
    t->pending_signal = 0;
    t->reaped = 0;
    t->exit_signal = 0;
    t->user_ticks = 0;
    t->sys_ticks = 0;
    t->child_user_ticks = 0;
    t->child_sys_ticks = 0;
    t->dead_thread_user_ticks = 0;
    t->dead_thread_sys_ticks = 0;
    t->max_rss_pages = 0;
    t->child_max_rss_pages = 0;
    for (int i = 0; i < PATH_MAX_LENGTH; i++) {
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
    t->cmdline_block = NULL;
    t->cmdline_length = 0;
    t->env_length = 0;
    t->env_count = 0;
    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = SIG_DFL_ADDR;
    }
    t->sig_restorer = 0;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_siginfo = 0;
    t->sig_resethand = 0;
    t->sig_onstack = 0;
    t->sig_alt_stack_base = 0;
    t->sig_alt_stack_size = 0;
    t->sig_on_alt_stack = 0;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_address = 0;
    scheduler_regions_release(t);
    t->fs_base = 0;
    t->tgid = thread_of ? thread_of->tgid : t->id;
    t->is_thread = thread_of ? 1 : 0;
    t->group_peak = 0;
    t->spawned_id = 0;
    t->spawned_caps = 0;
    t->detached = 0;
    t->exiting = 0;
    t->ending = 0;
    t->caps = caller->caps;
    /* M225: everything the spawner has to give a new process, given here,
       before the slot is published. M225 had the child wait at the door
       (user_launch_arguments_t::held) while the spawner wrote its command
       line, environment and narrowed capabilities AFTER it was runnable,
       and the door held only the child's user mode: the child could still
       be ended by a signal while it waited, reaped and its slot refilled,
       and the spawner's writes - capabilities included - then landed on the
       newcomer; and a spawner killed in that stretch left the child spinning
       at the door for good, since nobody else ever opened it. */
    if (setup) {
        t->caps &= setup->caps;
        t->cmdline_block = setup->cmdline;
        t->cmdline_length = setup->cmdline ? setup->cmdline_length : 0;
        t->env_block = setup->env;
        t->env_length = setup->env ? setup->env_length : 0;
        t->env_count = setup->env ? setup->env_count : 0;
    }
    t->prio = PRIO_INTERACTIVE;
    t->full_slices = 0;
    t->last_block_tick = pit_get_ticks();

    fpu_state_init(t->fpu_state);

    t->heap_brk = heap_start;
    t->heap_mapped_end = heap_start;
    t->shared_memory_next_vaddr = shared_memory_base;
    t->seen_exit_sequence = scheduler_exit_sequence();
    set_task_name(t, name);
    if (thread_of) {
        /* What a thread runs is its process's program - given here, before
           it can run, rather than copied in by its creator afterwards. */
        copy_task_name(t->program, thread_of->program);
    }

    uint64_t *sp = (uint64_t *)t->kernel_stack_top;
    *(--sp) = (uint64_t)task_entry_trampoline;
    *(--sp) = 0x2;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    t->rsp = (uint64_t)sp;

    {
        int live = 0;
        int together = 0;
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].state != TASK_FREE) {
                live++;
            }
            if (tasks[i].state != TASK_FREE && tasks[i].state != TASK_TERMINATED &&
                tasks[i].tgid == t->tgid) {
                together++;
            }
        }
        if (live > peak_live_tasks) {
            peak_live_tasks = live;
        }
        /* M225: how many of this process's tasks have been alive at once,
           kept on its leader - counted here, under the lock, at the only
           moment the number can grow. */
        task_t *leader = &tasks[PID_SLOT(t->tgid)];
        if (leader->tgid == t->tgid && (uint32_t)together > leader->group_peak) {
            leader->group_peak = (uint32_t)together;
        }
    }
    /* M225: the spawner's record of what it made, written while the slot
       cannot change - the child may run, end and be swept before the
       spawner reads anything back out of the task_t this returns. */
    caller->spawned_id = t->id;
    caller->spawned_caps = t->caps;

    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return t;
}

int scheduler_last_spawn(uint32_t *caps_out) {
    task_t *self = scheduler_current();
    if (!self) {
        return 0;
    }
    if (caps_out) {
        *caps_out = self->spawned_caps;
    }
    return self->spawned_id;
}


task_t *task_spawn(const char *name, void (*entry)(void *arg), void *arg) {
    return task_spawn_common(name, virtual_memory_kernel_pml4_phys(), entry, arg, 0, 0, (task_t *)0,
                             (const task_spawn_setup_t *)0);
}

task_t *task_spawn_in(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                       uint64_t heap_start, uint64_t shared_memory_base) {
    return task_spawn_common(name, pml4_phys, entry, arg, heap_start, shared_memory_base, (task_t *)0,
                             (const task_spawn_setup_t *)0);
}

task_t *task_spawn_program(const char *name, uint64_t pml4_phys, void (*entry)(void *arg),
                           void *arg, uint64_t heap_start, uint64_t shared_memory_base,
                           const task_spawn_setup_t *setup) {
    return task_spawn_common(name, pml4_phys, entry, arg, heap_start, shared_memory_base, (task_t *)0,
                             setup);
}

static int current_on_some_cpu(const task_t *t) {
    for (int c = 0; c < MAX_CPUS; c++) {
        if (current_task[c] == t) {
            return 1;
        }
    }
    return 0;
}

static task_t *pick_next(task_t *from, int cpu) {
    int start = PID_SLOT(from->id);
    task_t *idle = (task_t *)0;
    task_t *batch = (task_t *)0;
    for (int offset = 1; offset <= task_count; offset++) {
        int i = (start + offset) % task_count;
        if (tasks[i].state != TASK_READY) {
            continue;
        }
        if (&tasks[i] != from && current_on_some_cpu(&tasks[i])) {
            continue;
        }
        /* M175: current_on_some_cpu only refuses a task somebody is running
           right now. A stack-bound task is unavailable even when idle, because
           what it would hand over is another CPU's stack rather than a task. */
        if (tasks[i].home_cpu >= 0 && tasks[i].home_cpu != cpu) {
            continue;
        }
        if (tasks[i].prio == PRIO_BATCH && !tasks[i].is_idle) {
            if (!batch) {
                batch = &tasks[i];
            }
            continue;
        }
        if (tasks[i].is_idle) {
            if (!idle) {
                idle = &tasks[i];
            }
            continue;
        }
        return &tasks[i];
    }
    if (batch) {
        return batch;
    }
    if (!from->is_idle && (from->state == TASK_RUNNING || from->state == TASK_READY)) {
        return from;
    }
    return idle ? idle : from;
}

static void fire_expired_alarms(uint64_t now_ms) {
    /* M225: raised where the deadline is found, under the same hold of the
       lock. It looked the id up again afterwards and then raised through the
       door that checks none, so a task that ended and was replaced between
       the two took somebody else's SIGALRM. */
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    int woken = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_FREE && tasks[i].state != TASK_TERMINATED &&
            !tasks[i].is_idle && tasks[i].alarm_deadline_ms != 0 &&
            now_ms >= tasks[i].alarm_deadline_ms) {
            tasks[i].alarm_deadline_ms = 0;
            woken += raise_signal_locked(&tasks[i], SIGALRM);
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
}

unsigned int scheduler_set_alarm(task_t *t, unsigned int seconds) {
    if (!t) {
        return 0;
    }
    uint64_t now_ms = clock_monotonic_ms();
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    unsigned int remaining = 0;
    if (t->alarm_deadline_ms > now_ms) {
        remaining = (unsigned int)((t->alarm_deadline_ms - now_ms + 999) / 1000);
    }
    t->alarm_deadline_ms = seconds ? now_ms + (uint64_t)seconds * 1000 : 0;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return remaining;
}

static int waking_from_timer;

static int wake_expired_until(uint64_t now_ms, uint64_t *next_deadline_ms) {
    int woken = 0;
    uint64_t next = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    if (blocked_count == 0) {
        spin_unlock(&scheduler_lock);
        irq_restore(flags);
        *next_deadline_ms = 0;
        return 0;
    }
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_BLOCKED || tasks[i].wake_deadline_ms == 0) {
            continue;
        }
        if (now_ms >= tasks[i].wake_deadline_ms) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            tasks[i].ready_since_ms = clock_monotonic_ms();
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].last_block_tick = pit_get_ticks();
            need_resched[smp_current_cpu()] = 1;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
            tasks[i].woken_at_ns = clock_monotonic_ns();
            tasks[i].woken_by_timer = (uint8_t)waking_from_timer;
            woken++;
        } else if (next == 0 || tasks[i].wake_deadline_ms < next) {
            next = tasks[i].wake_deadline_ms;
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
    *next_deadline_ms = next;
    return woken;
}

/* Interrupts are off wherever this is called: the timer belongs to the core
   it is programmed on, and the task that asked must not move in between.
   A core keeps only its earliest deadline armed; a later one is found again
   when that fires, from the same scan that wakes the sleepers. */
static void arm_deadline_timer(int cpu, uint64_t deadline_ms) {
    if (deadline_ms == 0 || cpu < 0 || cpu >= MAX_CPUS || !lapic_timer_available()) {
        return;
    }
    uint64_t armed = armed_deadline_ms[cpu];
    if (armed != 0 && armed <= deadline_ms) {
        return;
    }
    armed_deadline_ms[cpu] = deadline_ms;
    uint64_t now_ns = clock_monotonic_ns();
    uint64_t at_ns = deadline_ms * 1000000ULL;
    lapic_timer_arm_ns(at_ns > now_ns ? at_ns - now_ns : 0);
}

void scheduler_deadline_timer_fired(void) {
    int cpu = smp_current_cpu();
    armed_deadline_ms[cpu] = 0;
    deadline_timer_interrupts++;
    uint64_t now_ms = clock_monotonic_ms();
    uint64_t next = 0;
    waking_from_timer = 1;
    int woken = wake_expired_until(now_ms, &next);
    waking_from_timer = 0;
    if (next != 0) {
        arm_deadline_timer(cpu, next);
    }
    task_t *t = current_task[cpu];
    if (woken > 0 && t && t->is_idle) {
        ticks_in_slice[cpu] = 0;
        schedule();
    }
}


uint64_t scheduler_deadline_timer_interrupts(void) {
    return deadline_timer_interrupts;
}

uint64_t scheduler_deadline_timer_lost(void) {
    return lost_deadline_arms;
}

/* A task made READY used to wait for some idle core's own tick - and an
   idle core rescheduled only when its quantum ran out, so up to twenty
   milliseconds - before anything ran it. A browser is dozens of threads in
   several processes handing messages to each other, and every hand-off paid
   that wait: on eight real cores the log showed ready work sitting beside
   idle processors a third of the time. Now the waker sends a sleeping core an
   IPI and the woken task runs within microseconds. One core per woken task,
   and not one that has already been asked and has not yet looked. */
static volatile uint8_t kick_pending[MAX_CPUS];

static void kick_idle_cpus(int wanted) {
    if (wanted <= 0 || !smp_is_initialized() || smp_cpu_count < 2) {
        return;
    }
    uint64_t flags = irq_save_disable();
    int self = smp_current_cpu();
    irq_restore(flags);
    for (int c = 0; c < smp_cpu_count && c < MAX_CPUS && wanted > 0; c++) {
        if (c == self) {
            continue;
        }
        task_t *running = current_task[c];
        if (!running || !running->is_idle) {
            continue;
        }
        if (__atomic_exchange_n(&kick_pending[c], 1, __ATOMIC_ACQ_REL)) {
            continue;
        }
        smp_send_reschedule(c);
        wanted--;
    }
}

void scheduler_reschedule_if_idle(void) {
    int cpu = smp_current_cpu();
    __atomic_store_n(&kick_pending[cpu], 0, __ATOMIC_RELEASE);
    task_t *t = current_task[cpu];
    if (t && t->is_idle) {
        schedule();
    }
}

uint64_t scheduler_event_sequence(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    uint64_t v = event_sequence;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return v;
}

/* M197. A futex is a user address, and every process running /bin/chrome
   has the same addresses: its image is loaded at one place and nothing
   randomises it. Keyed on the address alone, a FUTEX_WAKE of one waiter in
   one process could take a thread of ANOTHER process off its word - a
   spurious wake there and a lost one here, whose waiter then slept to its
   timeout. The key is the address AND the address space that owns it, which
   is what Linux means by a private futex. It also no longer bumps the poll
   sequence: nothing that waits on the sequence waits for a futex. */
int scheduler_wake_n(const void *chan, uint64_t space, int max) {
    if (!chan || max <= 0) {
        return 0;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    int woken = 0;
    for (int i = 0; i < task_count && woken < max; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan &&
            tasks[i].wait_space == space) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            tasks[i].ready_since_ms = clock_monotonic_ms();
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
            woken++;
        }
    }
    if (woken > 0) {
        need_resched[smp_current_cpu()] = 1;
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
    return woken;
}

/* M197. Every poller on the machine - epoll_wait, waitfds, a blocking read
   of a pipe or a socket - slept on ONE channel, and every pipe write, eventfd
   poke, socket send and timer change woke ALL of them: one Chromium thread
   posting a task woke every message pump in every process, the compositor
   and every desktop application, and each rescanned its descriptors and went
   back to sleep. Worse, a sleeper that saw the global sequence move between
   its scan and its sleep did not sleep at all, and on eight busy cores the
   sequence always moved - so pollers spun through the system call.

   A poller now says which kernel objects it is waiting on BEFORE it looks at
   them (scheduler_watch_begin/add), and a producer names the object it
   changed (scheduler_wake_object). A watcher that is already asleep is woken;
   one that has not got to sleep yet has watch_fired set and will not sleep.
   The object's own lock orders the two sides: the poller publishes its watch
   and then takes the object's lock to look; the producer changes the object
   under that lock and then takes the scheduler lock to read the watches. So
   either the poller's look sees the change or the producer sees the watch.

   A broadcast - task exit, input, anything that has no single object - still
   reaches every poller, and a poller that has not said what it watches (the
   paths not converted) is still woken by any object's change, exactly as
   before. */
static int watch_matches(const task_t *t, const void *first, const void *second) {
    if (!first || t->watch_everything) {
        return 1;
    }
    uint32_t count = __atomic_load_n(&t->watch_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count && i < SCHEDULER_WATCH_MAX; i++) {
        const void *object = t->watch_objects[i];
        if (object == first || (second && object == second)) {
            return 1;
        }
    }
    return 0;
}

static int wake_pollers_locked(const void *first, const void *second) {
    int woken = 0;
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        int blocked_here = t->state == TASK_BLOCKED && t->wait_chan == SCHEDULER_POLL_CHAN;
        if (!t->watching) {
            if (!blocked_here) {
                continue;
            }
        } else if (!watch_matches(t, first, second)) {
            continue;
        } else if (!blocked_here) {
            t->watch_fired = 1;
            continue;
        }
        t->watch_fired = 1;
        blocked_count--;
        t->state = TASK_READY;
        t->ready_since_ms = clock_monotonic_ms();
        t->prio = PRIO_INTERACTIVE;
        t->full_slices = 0;
        t->wait_chan = (const void *)0;
        t->wake_deadline_ms = 0;
        woken++;
    }
    return woken;
}

void scheduler_wake_objects(const void *first, const void *second) {
    if (!first) {
        first = second;
        second = (const void *)0;
    }
    if (!first) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    event_sequence++;
    int woken = wake_pollers_locked(first, second);
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
}

void scheduler_wake_object(const void *object) {
    scheduler_wake_objects(object, (const void *)0);
}

void scheduler_watch_begin(void) {
    uint64_t flags = irq_save_disable();
    task_t *self = current_task[smp_current_cpu()];
    spin_lock(&scheduler_lock);
    self->watch_count = 0;
    self->watch_everything = 0;
    self->watch_fired = 0;
    self->watching = 1;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
}

void scheduler_watch_add(const void *object) {
    task_t *self = current_task_now();
    if (!object || !self->watching) {
        return;
    }
    uint32_t count = self->watch_count;
    for (uint32_t i = 0; i < count; i++) {
        if (self->watch_objects[i] == object) {
            return;
        }
    }
    if (count >= SCHEDULER_WATCH_MAX) {
        __atomic_store_n(&self->watch_everything, 1, __ATOMIC_SEQ_CST);
        return;
    }
    self->watch_objects[count] = object;
    __atomic_store_n(&self->watch_count, count + 1, __ATOMIC_SEQ_CST);
}

/* A task that can sleep sleeps; the only callers that cannot - the boot
   context before there is anybody to switch to, and a CPU's idle identity -
   spin, and they only exist before the machine is running or never touch a
   lock like this at all. A waiter must never spin with interrupts off here:
   the holder runs with them on, so it can be preempted on this very core. */
void sleep_lock_acquire(sleep_lock_t *lock) {
    uint64_t flags = spin_lock_irqsave(&lock->guard);
    while (lock->held) {
        task_t *self = current_task[smp_current_cpu()];
        if (!self || self->is_idle || task_count <= 1) {
            spin_unlock_irqrestore(&lock->guard, flags);
            CPU_RELAX();
            flags = spin_lock_irqsave(&lock->guard);
            continue;
        }
        /* Not interruptible. Delivering a signal here can end the task, and
           a task waiting for one of these locks may already hold another -
           an exec holds the image cache's while it waits for the
           filesystem's - so a SIGKILL that arrived mid-exec left the image
           cache locked and every exec after it asleep for good (the boot
           battery hung at the first exec after [m167] killed its browser).
           A signal still wakes the waiter; it looks at the lock again and is
           delivered on the way back to user space, as it would be anyway. */
        lock->waiters++;
        if (self->sleep_lock_waiting != lock) {
            self->sleep_lock_wait_since_ms = clock_monotonic_ms();
            self->lock_wait_reported = 0;
        }
        self->sleep_lock_waiting = lock;
        block_on(lock, 0, 0, &lock->guard, &flags, 0);
        lock->waiters--;
    }
    if (current_task[smp_current_cpu()]) {
        current_task[smp_current_cpu()]->sleep_lock_waiting = (const void *)0;
    }
    lock->held = 1;
    task_t *owner = current_task[smp_current_cpu()];
    lock->holder = owner ? owner->id : -1;
    /* M198. The holder runs with interrupts on, and the tick delivers a
       pending fatal signal to whatever it interrupts - kernel mode included.
       A spinlock taken with interrupts off was never interrupted; this is,
       and a SIGKILL that landed while an exec held the image cache's lock
       ended the task holding it, and every exec after that waited for a
       task that no longer existed (the boot battery stopped at the first
       exec after [m167] killed its browser mid-launch). While a task holds
       one of these, its signals wait for it to let go. */
    if (owner) {
        owner->sleep_locks_held++;
    }
    spin_unlock_irqrestore(&lock->guard, flags);
}

void sleep_lock_release(sleep_lock_t *lock) {
    uint64_t flags = spin_lock_irqsave(&lock->guard);
    lock->held = 0;
    lock->holder = 0;
    task_t *owner = current_task[smp_current_cpu()];
    if (owner && owner->sleep_locks_held > 0) {
        owner->sleep_locks_held--;
    }
    if (lock->waiters > 0) {
        scheduler_wake_n(lock, 0, 1);
    }
    spin_unlock_irqrestore(&lock->guard, flags);
}

void scheduler_watch_end(void) {
    current_task_now()->watching = 0;
}

void scheduler_watch_block(uint64_t deadline_ms) {
    scheduler_deliver_pending_signal();
    scheduler_take_pending_stop_if_any(current_task_now());

    uint64_t sflags = irq_save_disable();
    int cpu = smp_current_cpu();
    spin_lock(&scheduler_lock);
    task_t *self = current_task[cpu];
    if (self->watch_fired) {
        self->watching = 0;
        spin_unlock(&scheduler_lock);
        irq_restore(sflags);
        return;
    }
    self->wait_chan = SCHEDULER_POLL_CHAN;
    self->wake_deadline_ms = deadline_ms;
    arm_deadline_timer(cpu, deadline_ms);
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&scheduler_lock);
    irq_restore(sflags);

    schedule();
    unblock_self(self);
    self->watching = 0;
}

void scheduler_wake_all(const void *chan) {
    if (!chan) {
        return;
    }
    if (chan == SCHEDULER_POLL_CHAN) {
        uint64_t pflags = irq_save_disable();
        spin_lock(&scheduler_lock);
        event_sequence++;
        int woken = wake_pollers_locked((const void *)0, (const void *)0);
        spin_unlock(&scheduler_lock);
        irq_restore(pflags);
        kick_idle_cpus(woken);
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    event_sequence++;
    if (blocked_count == 0) {
        spin_unlock(&scheduler_lock);
        irq_restore(flags);
        return;
    }
    int woken = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            tasks[i].ready_since_ms = clock_monotonic_ms();
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
            woken++;
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
}

static void unblock_self(task_t *self) {
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    if (self->state == TASK_BLOCKED) {
        self->state = TASK_RUNNING;
        blocked_count--;
    }
    self->wait_chan = (const void *)0;
    self->wake_deadline_ms = 0;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
}

void scheduler_block_on_sequence(const void *chan, uint64_t deadline_ms, uint64_t expected_sequence) {
    scheduler_deliver_pending_signal();
    scheduler_take_pending_stop_if_any(current_task_now());

    uint64_t sflags = irq_save_disable();
    int cpu = smp_current_cpu();
    spin_lock(&scheduler_lock);
    if (event_sequence != expected_sequence) {
        spin_unlock(&scheduler_lock);
        irq_restore(sflags);
        return;
    }
    task_t *self = current_task[cpu];
    self->wait_chan = chan;
    self->wake_deadline_ms = deadline_ms;
    arm_deadline_timer(cpu, deadline_ms);
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&scheduler_lock);
    irq_restore(sflags);

    schedule();
    unblock_self(self);
}

/* Sleep this task, and give the machine to whatever else wants it.

   pit_sleep_ms() halts in a loop and leaves the caller RUNNABLE, so a task
   that sleeps that way goes on being picked and spends its whole slice in
   hlt. On an idle machine that costs nothing, which is why it went unnoticed
   for a hundred and sixty milestones. On a busy one it costs a share of the
   CPU per sleeper: measured, the TCP retransmit timer - which sleeps 100 ms
   at a time and does a few microseconds of work between - burned 29 seconds
   of processor during a 58-second self-test stage, and 150 seconds before it.
   The stage costs 26 seconds on a machine with nothing else on it.

   Blocking is what the scheduler already has for this; nothing here is new
   except which primitive the sleeper reaches for. It returns early if a
   signal is pending, which is what a sleeping task should do. */
static const char sleep_channel;

void scheduler_sleep_ms(uint32_t ms) {
    task_t *self = current_task_now();
    if (!self || self->is_idle) {
        return;
    }
    uint64_t deadline = clock_deadline_ms((uint64_t)(ms ? ms : 1));
    uint64_t sflags = irq_save_disable();
    spin_lock(&scheduler_lock);
    self->wait_chan = (const void *)&sleep_channel;
    self->wake_deadline_ms = deadline;
    arm_deadline_timer(smp_current_cpu(), deadline);
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&scheduler_lock);
    irq_restore(sflags);

    scheduler_deliver_pending_signal();
    schedule();
    unblock_self(self);
}

void scheduler_block_on(const void *chan, uint64_t deadline_ms, spinlock_t *lock, uint64_t *flags) {
    scheduler_block_on_in_space(chan, 0, deadline_ms, lock, flags);
}

void scheduler_block_on_in_space(const void *chan, uint64_t space, uint64_t deadline_ms,
                                 spinlock_t *lock, uint64_t *flags) {
    block_on(chan, space, deadline_ms, lock, flags, 1);
}

static void block_on(const void *chan, uint64_t space, uint64_t deadline_ms, spinlock_t *lock,
                     uint64_t *flags, int interruptible) {
    uint64_t sflags = irq_save_disable();
    int cpu = smp_current_cpu();
    spin_lock(&scheduler_lock);
    task_t *self = current_task[cpu];
    self->wait_chan = chan;
    self->wait_space = space;
    self->wake_deadline_ms = deadline_ms;
    arm_deadline_timer(cpu, deadline_ms);
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&scheduler_lock);
    irq_restore(sflags);

    spin_unlock_irqrestore(lock, *flags);

    if (interruptible) {
        scheduler_deliver_pending_signal();
    }

    schedule();
    unblock_self(self);
    *flags = spin_lock_irqsave(lock);
}

void schedule(void) {
    uint64_t flags = irq_save_disable();
    int cpu = smp_current_cpu();
    spin_lock(&scheduler_lock);
    task_t *previous = current_task[cpu];
    task_t *next = pick_next(previous, cpu);

    if (next == previous) {
        spin_unlock(&scheduler_lock);
        irq_restore(flags);
        return;
    }
    close_halt_interval(cpu);

    for (int c = 0; c < MAX_CPUS; c++) {
        if (c != cpu && current_task[c] == next) {
            kernel_log_puts("[sched] cpu ");
            kernel_log_put_dec((uint32_t)cpu);
            kernel_log_puts(" picked task '");
            kernel_log_puts(next->name[0] ? next->name : "(unnamed)");
            kernel_log_puts("' which cpu ");
            kernel_log_put_dec((uint32_t)c);
            kernel_log_puts(" is already running\n");
            panic("sched: one task, two CPUs - a stack with two owners");
        }
    }

    if (previous->state == TASK_RUNNING) {
        previous->state = TASK_READY;
        previous->ready_since_ms = clock_monotonic_ms();
    }
    next->state = TASK_RUNNING;
    current_task[cpu] = next;

    tss_set_rsp0(cpu, next->kernel_stack_top);
    /* M225: a task part way through its exit keeps NAMING its address space
       (a leader's pml4_phys is how its surviving threads find theirs), but
       it never runs on it again: the last thread out may already have
       destroyed it (task_exit_with_code). */
    uint64_t next_space = next->exiting ? virtual_memory_kernel_pml4_phys() : next->pml4_phys;
    if (next_space != loaded_pml4_phys[cpu]) {
        /* Published BEFORE the load, and fenced: a core that changes this
           address space's page tables reads this array afterwards to decide
           whom to tell, so a core that is about to load it must already be
           visible here - otherwise it could fill its TLB from the old entries
           in the gap and never be asked to drop them. */
        __atomic_store_n(&loaded_pml4_phys[cpu], next_space, __ATOMIC_SEQ_CST);
        virtual_memory_switch_address_space(next_space);
    }

    fpu_save(previous->fpu_state);
    fpu_restore(next->fpu_state);

    cpu_write_msr(MSR_FS_BASE, next->fs_base);

    if (previous->home_cpu >= 0 && previous->home_cpu != cpu) {
        kernel_log_puts("[sched] cpu ");
        kernel_log_put_dec((uint32_t)cpu);
        kernel_log_puts(" was running '");
        kernel_log_puts(previous->name[0] ? previous->name : "(unnamed)");
        kernel_log_puts("', which is bound to the stack of cpu ");
        kernel_log_put_dec((uint32_t)previous->home_cpu);
        kernel_log_putc('\n');
        scheduler_dump_cpus();
        panic("sched: a stack-bound task ran on a CPU that is not its own");
    }

    if (previous->kernel_stack_top != 0) {
        uint64_t sp = cpu_stack_pointer();
        uint64_t base = (uint64_t)(uintptr_t)previous->stack_base;
        if (sp < base || sp >= previous->kernel_stack_top) {
            kernel_log_puts("[sched] cpu ");
            kernel_log_put_dec((uint32_t)cpu);
            kernel_log_puts(" is switching away from '");
            kernel_log_puts(previous->name[0] ? previous->name : "(unnamed)");
            kernel_log_puts("' (stack 0x");
            kernel_log_put_hex64(base);
            kernel_log_puts("..0x");
            kernel_log_put_hex64(previous->kernel_stack_top);
            kernel_log_puts(") while standing on rsp=0x");
            kernel_log_put_hex64(sp);
            kernel_log_putc('\n');
            scheduler_dump_cpus();
            panic("sched: this CPU is not on the stack of the task it thinks it is running");
        }
    }

    /* M182: and the other end of the same question, asked of the task being
       resumed. M106's check above fires when a CPU is standing somewhere its
       current task's stack does not cover - which is where the damage is SEEN,
       not where it is done: the rsp it complains about was written into that
       task by an earlier context_switch saving it, so by then the first event
       is long gone.

       This one looks at next->rsp before jumping to it. A task whose saved
       stack pointer is not inside its own stack has never run there, and the
       task it DOES point into is named, which is the pair the other check
       could only guess at. It costs two comparisons on a path that already
       does an FPU save and a page-table compare. */
    if (next->kernel_stack_top != 0) {
        uint64_t resume = next->rsp;
        uint64_t next_base = (uint64_t)(uintptr_t)next->stack_base;
        if (resume < next_base || resume >= next->kernel_stack_top) {
            kernel_log_puts("[sched] cpu ");
            kernel_log_put_dec((uint32_t)cpu);
            kernel_log_puts(" is resuming '");
            kernel_log_puts(next->name[0] ? next->name : "(unnamed)");
            kernel_log_puts("' pid 0x");
            kernel_log_put_hex32((uint32_t)next->id);
            kernel_log_puts(" at rsp=0x");
            kernel_log_put_hex64(resume);
            kernel_log_puts(", outside its own stack 0x");
            kernel_log_put_hex64(next_base);
            kernel_log_puts("..0x");
            kernel_log_put_hex64(next->kernel_stack_top);
            kernel_log_putc('\n');
            scheduler_dump_stack_owner(resume);
            scheduler_dump_cpus();
            panic("sched: a task's saved stack pointer is not in its own stack");
        }
    }

    switch_count[cpu]++;
    next->last_ran_ms = clock_monotonic_ms();
    next->starvation_reported = 0;
    context_switch(&previous->rsp, next->rsp);

    spin_unlock(&scheduler_lock);
    irq_restore(flags);
}

uint64_t scheduler_switch_count(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? switch_count[cpu] : 0;
}

/* M182: whose stack is this address in? A saved rsp that is not in its own
   task's stack is almost always in somebody else's, and which somebody is the
   whole question - it is the difference between "two cores shared a stack" and
   "a stack was freed and handed out again". */
void scheduler_dump_stack_owner(uint64_t address) {
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_FREE || t->kernel_stack_top == 0) {
            continue;
        }
        uint64_t base = (uint64_t)(uintptr_t)t->stack_base;
        if (address >= base && address < t->kernel_stack_top) {
            kernel_log_puts("  that address is in '");
            kernel_log_puts(t->name[0] ? t->name : "(unnamed)");
            kernel_log_puts("' pid 0x");
            kernel_log_put_hex32((uint32_t)t->id);
            kernel_log_puts(" state ");
            kernel_log_put_dec((uint32_t)t->state);
            kernel_log_puts(" stack 0x");
            kernel_log_put_hex64(base);
            kernel_log_puts("..0x");
            kernel_log_put_hex64(t->kernel_stack_top);
            kernel_log_puts(", ");
            kernel_log_put_dec((uint32_t)(t->kernel_stack_top - address));
            kernel_log_puts(" bytes below its top\n");
            return;
        }
    }
    kernel_log_puts("  that address is in no live task's stack - a freed one, "
               "or never a stack at all\n");
}

int scheduler_task_slot_count(void) {
    return task_count;
}

task_t *scheduler_task_slot(int index) {
    return (index >= 0 && index < task_count) ? &tasks[index] : (task_t *)0;
}

task_t *scheduler_cpu_current(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? current_task[cpu] : (task_t *)0;
}

void scheduler_dump_cpus(void) {
    for (int c = 0; c < MAX_CPUS; c++) {
        task_t *t = current_task[c];
        if (!t) {
            continue;
        }
        kernel_log_puts("  cpu");
        kernel_log_put_dec((uint32_t)c);
        kernel_log_puts("=");
        kernel_log_puts(t->name[0] ? t->name : "(unnamed)");
        kernel_log_puts("/0x");
        kernel_log_put_hex32((uint32_t)t->id);
        kernel_log_puts(" kstack=0x");
        kernel_log_put_hex64(t->kernel_stack_top);
    }
    kernel_log_putc('\n');
}

static void scheduler_deliver_pending_signal(void) {
    task_t *t = current_task_now();
    if (t->pending_signal != 0 && t->sleep_locks_held == 0 && !t->ending) {
        deliver_pending_signal_and_exit(t);
    }
}

/* How the last processes to end ended, after their parents have reaped
   them. The compositor is not the parent of most windows on the desktop -
   desktop_icons launched the README editor - and the zombie that held the
   answer is gone the moment its parent waits, so "did that window's program
   crash or quit" had no answer by the time the compositor asked, and it
   guessed "crashed" (M209). Threads are not recorded: nobody asks how a
   thread ended, and Chromium ends hundreds. */
#define RECENT_EXIT_COUNT 64

typedef struct {
    int pid;
    int status;
} recent_exit_t;

static recent_exit_t recent_exits[RECENT_EXIT_COUNT];
static int recent_exit_next;
static spinlock_t recent_exit_lock;

int scheduler_wait_status(const task_t *t) {
    if (t->exit_signal) {
        return t->exit_signal & 0x7F;
    }
    return (t->exit_code & 0xFF) << 8;
}

static void remember_exit(const task_t *t) {
    if (t->is_thread) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&recent_exit_lock);
    recent_exits[recent_exit_next].pid = t->id;
    recent_exits[recent_exit_next].status = scheduler_wait_status(t);
    recent_exit_next = (recent_exit_next + 1) % RECENT_EXIT_COUNT;
    spin_unlock_irqrestore(&recent_exit_lock, f);
}

void scheduler_log_recent_exits(void) {
    kernel_log_puts("[hang] recent process exits (pid, wait status):");
    uint64_t f = spin_lock_irqsave(&recent_exit_lock);
    for (int i = 0; i < RECENT_EXIT_COUNT; i++) {
        int at = (recent_exit_next + i) % RECENT_EXIT_COUNT;
        if (recent_exits[at].pid == 0) {
            continue;
        }
        kernel_log_puts(" ");
        kernel_log_put_dec((uint32_t)recent_exits[at].pid);
        kernel_log_puts("=0x");
        kernel_log_put_hex32((uint32_t)recent_exits[at].status);
    }
    spin_unlock_irqrestore(&recent_exit_lock, f);
    kernel_log_putc('\n');
}

int scheduler_recent_exit_status(int pid, int *status_out) {
    int found = 0;
    uint64_t f = spin_lock_irqsave(&recent_exit_lock);
    for (int i = 0; i < RECENT_EXIT_COUNT && !found; i++) {
        if (recent_exits[i].pid == pid && pid != 0) {
            *status_out = recent_exits[i].status;
            found = 1;
        }
    }
    spin_unlock_irqrestore(&recent_exit_lock, f);
    return found;
}

/* M225: how a process ended, whether or not its slot is still there. A child
   of the kernel is released by the next spawn's sweep the moment it has
   terminated (nobody_will_wait), and a self-test that spawns four writers
   and then waits for them loses any writer that finishes before the fourth
   spawn: its slot is gone, scheduler_task_by_id says nobody, and its exit
   code went with it. The record of recent exits outlives the slot, so the
   answer is still here.

   The first version of this read the slot with no lock - so a sweep on
   another processor could release it and a spawn refill it between the
   generation check and the read, and the answer was the newcomer's - and it
   answered in two encodings: the slot's whole exit_code, the record's low
   eight bits. An exit(256) read 256 and then, once the sweep had run, 0.
   Both now come from the wait status, the one encoding waitpid reports,
   and the slot is looked at under the lock that releasing it takes. */
int scheduler_exit_code_of_status(int status) {
    return (status & 0x7F) ? 128 + (status & 0x7F) : (status >> 8) & 0xFF;
}

int scheduler_exit_status(int pid, int *status_out) {
    if (pid <= 0) {
        return -1;
    }
    int answer = -1;
    int status = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    int slot = PID_SLOT(pid);
    if (slot < task_count) {
        const task_t *t = &tasks[slot];
        if (t->state != TASK_FREE && t->generation == PID_GEN(pid)) {
            /* Ended as wait() says it: the whole process (M225). */
            if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != TASK_TERMINATED ||
                (!t->is_thread && group_has_live_members_locked(t->tgid, t))) {
                answer = 0;
            } else {
                status = scheduler_wait_status(t);
                answer = 1;
            }
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    if (answer < 0 && scheduler_recent_exit_status(pid, &status)) {
        /* Released - and so terminated, and remember_exit ran before the
           slot was ever marked TERMINATED. */
        answer = 1;
    }
    if (answer == 1 && status_out) {
        *status_out = status;
    }
    return answer;
}

int scheduler_exit_code(int pid, int *code_out) {
    int status = 0;
    int answer = scheduler_exit_status(pid, &status);
    if (answer == 1 && code_out) {
        *code_out = scheduler_exit_code_of_status(status);
    }
    return answer;
}

/* M224: a signal that terminates terminates the PROCESS. Each thread here is
   a task, and this ended only the one the signal reached - so SIGTERM to a
   Node process ended its main thread and left the platform workers, the
   libuv pool and the inspector thread running with no main thread, holding
   every descriptor the process had. A parent reading that child's stdout
   waited for an EOF that never came. exit() has always taken the group with
   it; death by a signal does the same now. */
/* M225: from here on no signal reaches this task (task_t::ending). The ones
   already pending are dropped with it: a fatal one has nothing left to end,
   and a stop would park a task half way out. Called FIRST by every way out -
   before the exit sends SIGKILL to the rest of the process, because the
   first of them to die sends one straight back, and on another processor it
   can arrive before this task has got any further. */
void scheduler_begin_exit(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t eflags = irq_save_disable();
    spin_lock(&scheduler_lock);
    t->ending = 1;
    t->pending_signal = 0;
    t->pending_stop = 0;
    spin_unlock(&scheduler_lock);
    irq_restore(eflags);
}

void task_exit_with_signal(int sig) {
    task_t *t = current_task_now();
    if (t) {
        scheduler_begin_exit(t);
        t->exit_signal = sig;
        scheduler_kill_thread_group(t);
    }
    task_exit_with_code(128 + sig);
}

void task_exit_with_code(int code) {
    task_t *t = current_task_now();
    scheduler_begin_exit(t);
    shared_memory_free_by_owner(t->id);
    /* M225 (fd-use-holds): a task can end inside a system call - a fatal
       signal is acted on in a blocking wait or from the tick - and what that
       call held is given back here, before the table: a reader killed while
       blocked on a pipe must not keep the pipe's read end open for ever. */
    file_descriptor_put_all(t);
    scheduler_release_file_descriptors(t);

    if (t->sid != 0 && t->sid == t->id) {
        tty_release_session(tty_console(), t->sid);
        pty_release_session(t->sid);
    }

    if (t->pml4_phys != virtual_memory_kernel_pml4_phys()) {
        uint64_t dead = t->pml4_phys;
        uint64_t peak = virtual_memory_rss_peak_pages(dead);
        if (peak > t->max_rss_pages) {
            t->max_rss_pages = peak;
        }
        int others = 0;
        uint64_t eflags = irq_save_disable();
        spin_lock(&scheduler_lock);
        /* M225: off the address space in the same breath as saying so. Two
           threads of a process exiting at once each skip the other once it
           is marked, so the second destroys the space - and the first, which
           saw the second still live and so was not last, went on running
           its exit with CR3 on a page table that had been freed and could be
           handed to anybody, until its schedule() moved it off. exit() makes
           that the common case: it SIGKILLs every sibling at once. A task
           marked exiting is never on the space again: this core leaves it
           here, under the lock the other exiters read the mark under, and a
           switch back to an exiting task loads the kernel's (switch_to). */
        t->exiting = 1;
        __atomic_store_n(&loaded_pml4_phys[smp_current_cpu()], virtual_memory_kernel_pml4_phys(),
                         __ATOMIC_SEQ_CST);
        virtual_memory_switch_address_space(virtual_memory_kernel_pml4_phys());
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
        spin_unlock(&scheduler_lock);
        irq_restore(eflags);

        if (!others) {
            /* The regions belong to the ADDRESS SPACE, and the task standing
               here at the end need not be the one that owns them - a process
               whose main thread returned first is torn down by whichever
               thread is last. Reading an empty table then releases nothing,
               and the teardown below goes on to free the frames of every
               shared and memfd page still mapped - frames the memfd owns and
               will free again. The panic that follows says "double-free" and
               names a frame, which is three layers from the cause.

               M146 drew this line for descriptors, M165 for mappings and
               M166 for capabilities; this is the same one, on the path that
               only runs once per process and so was never the one being
               looked at. */
            task_t *space = scheduler_vm_owner(t);
            if (!space || space->pml4_phys != dead) {
                space = t;
            }
            scheduler_release_shared_range(space, USER_MMAP_BASE,
                                           USER_MMAP_LIMIT);
        }
        /* Only when this address space is going away. A task that is
           terminated but whose threads are still running MUST go on naming
           the space they share: scheduler_vm_owner() finds the leader, and
           everything that reaches for the address space through the owner -
           the mappings, the break, the page fault handler's own pml4 - would
           otherwise be handed the KERNEL's. Resetting it unconditionally was
           invisible while a process was its main thread, and is how a memfd
           page stayed mapped into an address space nobody could release it
           from. */
        if (!others) {
            scheduler_load_address_space(t, virtual_memory_kernel_pml4_phys());
            process_destroy_address_space(dead);
        }
    }

    t->exit_code = code;
    remember_exit(t);
    if (t->state == TASK_BLOCKED) {
        blocked_count--;
    }
    /* Release: whoever sees TERMINATED sees the exit code and signal above
       (scheduler_exit_status reads them on the strength of this).

       M225: and under the scheduler lock, because this is also where a
       PROCESS ends - when the last of its tasks does - and two of its tasks
       ending at once on two processors must agree which of them was last.
       Each looks for live members of the group with the lock held, after
       marking itself; the second to get here sees the first as terminated,
       so exactly one finds the group empty. What the parent is told is
       taken here too, while the leader's slot cannot be reaped and refilled
       from under it: wait() answers the moment the group is empty, and the
       slot is the waiter's to release from then on. */
    process_end_t ended = {0};
    {
        uint64_t eflags = irq_save_disable();
        spin_lock(&scheduler_lock);
        __atomic_store_n(&t->state, TASK_TERMINATED, __ATOMIC_RELEASE);
        task_t *leader = &tasks[PID_SLOT(t->tgid)];
        if (leader->id == t->tgid && !leader->is_thread &&
            leader->state == TASK_TERMINATED &&
            !group_has_live_members_locked(t->tgid, (const task_t *)0)) {
            ended.happened = 1;
            ended.leader = leader;
            ended.pid = leader->id;
            ended.parent_id = leader->parent_id;
            ended.status = leader->exit_signal ? (int32_t)(leader->exit_signal & 0x7F)
                                               : (int32_t)(leader->exit_code & 0xFF);
        }
        spin_unlock(&scheduler_lock);
        irq_restore(eflags);
    }
    __atomic_add_fetch(&exit_sequence, 1, __ATOMIC_RELEASE);
    scheduler_wake_all((const void *)t);
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    if (ended.happened && ended.leader != t) {
        /* The leader went first; whoever waits for the process waits on it. */
        scheduler_wake_all((const void *)ended.leader);
    }
    /* M225: SIGCHLD says a child process has ENDED, which is what wait()
       will then report - so it is sent when the process does, not when its
       leader does. A leader that returned first while its threads ran on
       (every Chromium child, M167's leaderexit) used to signal its parent
       at once; wait() reported it at once too, so they agreed, but both
       said "ended" about a process still running in its address space, and
       a parent's wait() returned with the child's exit() still killing its
       threads ([m205] on four processors). wait() now waits for the group
       to be empty, and a SIGCHLD sent before that would be answered by a
       WNOHANG that finds nothing to reap - libuv's way - and never again. */
    if (ended.happened && ended.parent_id >= 0) {
        raise_child_signal(ended.parent_id, ended.pid, ended.status);
    } else if (t->is_thread && t->parent_id >= 0) {
        /* A thread's parent is the process that made it, as it always was. */
        raise_child_signal(t->parent_id, t->id,
                           t->exit_signal ? (int32_t)(t->exit_signal & 0x7F)
                                          : (int32_t)(t->exit_code & 0xFF));
    }
    schedule();
    panic("task_exit: terminated task resumed");
}

void task_exit(void) {
    task_exit_with_code(0);
}

task_t *scheduler_current(void) {
    return current_task_now();
}

task_t *scheduler_task_by_id(int pid) {
    if (pid < 0) {
        return (task_t *)0;
    }
    int slot = PID_SLOT(pid);
    if (slot >= task_count) {
        return (task_t *)0;
    }
    task_t *t = &tasks[slot];
    if (t->state == TASK_FREE || t->generation != PID_GEN(pid)) {
        return (task_t *)0;
    }
    return t;
}

task_t *scheduler_task_by_slot(int slot) {
    if (slot < 0 || slot >= task_count || tasks[slot].state == TASK_FREE) {
        return (task_t *)0;
    }
    return &tasks[slot];
}

int scheduler_task_count(void) {
    return task_count;
}

int scheduler_peak_live_tasks(void) {
    return peak_live_tasks;
}

int scheduler_file_descriptor_high_water(int *which_task_out) {
    int best = 0;
    uint64_t f = spin_lock_irqsave(&scheduler_lock);
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE) {
            continue;
        }
        if (!tasks[i].descriptor_table) {
            continue;
        }
        int used = 0;
        for (int k = 0; k < MAX_FILE_DESCRIPTORS; k++) {
            if (tasks[i].descriptor_table->slots[k].type != FILE_DESCRIPTOR_NONE) {
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
    spin_unlock_irqrestore(&scheduler_lock, f);
    return best;
}

int scheduler_live_task_count(void) {
    int n = 0;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_FREE) {
            n++;
        }
    }
    return n;
}

int scheduler_has_free_task_slot(void) {
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

/* Whether any task of this thread group is still running.

   By GROUP and not by page table, though the question is about the address
   space. A tgid is a pid and carries its slot's generation, so it names one
   group for ever; a pml4_phys is a physical frame, and the frame of an
   address space that has been torn down is handed to the next process that
   asks for one. Comparing those made a terminated leader match a stranger
   that inherited its page table, and its slot was then held for ever - which
   on a busy machine is a task table that fills up, and a boot that stops
   getting anywhere rather than crashing. */
static int group_has_live_members_locked(int group, const task_t *except) {
    for (int i = 0; i < task_count; i++) {
        const task_t *o = &tasks[i];
        if (o == except || o->state == TASK_FREE || o->state == TASK_TERMINATED) {
            continue;
        }
        if (o->tgid == group) {
            return 1;
        }
    }
    return 0;
}

static int thread_group_has_live_members(int group, const task_t *except) {
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    int live = group_has_live_members_locked(group, except);
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return live;
}

/* M225: whether wait() may report this task's process as ended - its leader
   has terminated AND so has every thread it had. POSIX's "the process has
   terminated" is the whole process, and Linux holds a zombie leader back
   from wait() until its thread group is empty for the same reason. A thread
   (pthread_join's business, not wait's) has ended when it has. */
int scheduler_process_has_ended(const task_t *t) {
    if (!t || __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != TASK_TERMINATED) {
        return 0;
    }
    return t->is_thread || !thread_group_has_live_members(t->tgid, t);
}

static file_descriptor_table_t *take_descriptor_table(task_t *t);

void scheduler_reap_slot(task_t *t) {
    if (!t || t->state != TASK_TERMINATED) {
        return;
    }

    /* A thread-group leader's slot is not reset while its threads are still
       running in the address space it owns.

       Everything below belongs to the ADDRESS SPACE rather than to the task:
       the mapping table is freed, the memfd references it holds are
       forgotten, and the slot goes back with a new generation - after which
       scheduler_vm_owner() cannot find the owner at all, and every surviving
       thread is running in a space with no record of what is mapped in it.
       The teardown then frees the frames of a shared mapping a second time,
       at the same virtual address every run.

       M167 stopped a terminated leader from giving up its page table while
       its threads still ran. This is the same process one step further
       along, and it is not the leader that does it: the PARENT reaps it, so
       an address space loses its regions to a wait() in a different process
       entirely. Chromium's browser reaps a child whose main thread returned
       first, which is every child it has.

       Nothing is lost by holding the slot - wait() reads the exit code and
       sets `reaped` before calling this - and the slot comes back below, as
       soon as the last thread that was using the space is itself reaped. */
    if (!t->is_thread && thread_group_has_live_members(t->tgid, t)) {
        return;
    }
    /* M206: one reaper per slot. The state was read above without the lock,
       and between that read and the work below a second reaper - a spawn's
       sweep on another processor, or the same one after a preemption - could
       read it too: two of them freed the same kernel stack, which the next
       two tasks were then both given, and a terminated task came back to
       life on somebody else's stack. */
    {
        uint64_t claim = irq_save_disable();
        spin_lock(&scheduler_lock);
        int ours = t->state == TASK_TERMINATED && !t->reaping;
        if (ours) {
            t->reaping = 1;
        }
        spin_unlock(&scheduler_lock);
        irq_restore(claim);
        if (!ours) {
            return;
        }
    }
    /* M203: announced only once the slot really goes. It was announced
       before the check above, so every attempt to reap a leader whose
       threads were still running told every poller on the machine that a
       program had exited when none had - and woke the compositor, whose
       wait ends on exactly that. */
    __atomic_add_fetch(&exit_sequence, 1, __ATOMIC_RELEASE);
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    const int reaped_group = t->tgid;
    const int reaped_a_thread = t->is_thread;

    /* The command line belongs to the address space, so it goes when the last
       member of the group does - not at task_exit, where a leader that
       outlives its own main thread would take it away from threads that are
       still running and still have a /proc entry.

       M225 (process-lifetimes): and so does the environment record, for the
       same reason. Every reader of it asks the OWNER (scheduler_vm_owner) -
       a thread has none of its own - and task_exit freed the leader's while
       its threads ran on: a thread that spawned or exec'd after its main
       thread had returned read a freed block, or found none and gave its
       child an empty environment. */
    scheduler_release_cmdline(t);
    scheduler_release_env(t);

    /* task_exit_with_code released this on the way out and it is normally
       null by now. A task that reached TERMINATED without going through
       that path still holds one, and since M146 the table is a heap
       allocation rather than an array inside the task - so leaving it here
       is a leak rather than nothing. Releasing is idempotent; the pointer
       is taken under the lock and the reference dropped outside it, because
       closing a descriptor can reach a pipe or a socket, which is not work
       for a locked region. Through the same door as an exit, so a table
       whose last holder this was gives back its process's record locks. */
    scheduler_release_file_descriptors(t);
    uint64_t flags;
    {
        uint64_t deadline = pit_get_ticks() + PIT_HZ;
        for (;;) {
            flags = irq_save_disable();
            spin_lock(&scheduler_lock);
            int still_running = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                if (current_task[c] == t) {
                    still_running = 1;
                    break;
                }
            }
            if (!still_running) {
                break;
            }
            spin_unlock(&scheduler_lock);
            irq_restore(flags);
            if (pit_get_ticks() > deadline) {
                kernel_log_puts("[sched] '");
                kernel_log_puts(t->name[0] ? t->name : "(unnamed)");
                kernel_log_puts("' pid 0x");
                kernel_log_put_hex32((uint32_t)t->id);
                kernel_log_puts(" is terminated and still on a CPU\n");
                scheduler_dump_cpus();
                panic("sched_reap_slot: a terminated task never left its kernel stack");
            }
            cpu_spin_hint();
        }
    }
    task_t *parent = scheduler_task_by_id(t->parent_id);
    if (parent) {
        if (t->is_thread && t->tgid == parent->tgid) {
            /* A dead thread's processor time belongs to the PROCESS, not to
               whichever thread happened to create it. Adding it to the
               parent's own counters would have made that thread's
               CLOCK_THREAD_CPUTIME_ID jump every time one of its children was
               joined, so it goes to an accumulator on the group leader that
               only scheduler_thread_group_ticks reads. */
            task_t *leader = scheduler_task_by_id(t->tgid);
            if (leader) {
                leader->dead_thread_user_ticks += t->user_ticks;
                leader->dead_thread_sys_ticks += t->sys_ticks;
            }
            parent->child_user_ticks += t->child_user_ticks;
            parent->child_sys_ticks += t->child_sys_ticks;
            if (t->max_rss_pages > parent->max_rss_pages) {
                parent->max_rss_pages = t->max_rss_pages;
            }
            if (t->child_max_rss_pages > parent->child_max_rss_pages) {
                parent->child_max_rss_pages = t->child_max_rss_pages;
            }
        } else {
            parent->child_user_ticks += t->user_ticks + t->child_user_ticks;
            parent->child_sys_ticks += t->sys_ticks + t->child_sys_ticks;
            if (t->max_rss_pages > parent->child_max_rss_pages) {
                parent->child_max_rss_pages = t->max_rss_pages;
            }
            if (t->child_max_rss_pages > parent->child_max_rss_pages) {
                parent->child_max_rss_pages = t->child_max_rss_pages;
            }
        }
    }

    if (!reaped_a_thread) {
        for (int i = 0; i < task_count; i++) {
            task_t *o = &tasks[i];
            if (o != t && o->state != TASK_FREE && o->lineage_id == reaped_group) {
                o->lineage_id = t->lineage_id;
            }
        }
    }

    uint8_t *stack = t->stack_base;
    t->stack_base = NULL;
    t->kernel_stack_top = 0;
    t->home_cpu = -1;
    t->kernel_activity = KERNEL_ACTIVITY_NONE;
    t->syscalls = 0;
    t->descriptor_uses = (file_descriptor_use_t *)0;
    t->stamp_syscalls_seen = 0;
    t->generation++;
    t->state = TASK_FREE;
    t->reaping = 0;
    t->pending_signal = 0;
    t->pending_stop = 0;
    t->stopped_sig = 0;
    t->stop_reported = 0;
    t->reaped = 0;
    t->user_ticks = 0;
    t->sys_ticks = 0;
    t->child_user_ticks = 0;
    t->child_sys_ticks = 0;
    t->dead_thread_user_ticks = 0;
    t->dead_thread_sys_ticks = 0;
    t->max_rss_pages = 0;
    t->child_max_rss_pages = 0;
    t->exit_signal = 0;
    t->exit_code = 0;
    t->prio = PRIO_INTERACTIVE;
    t->full_slices = 0;
    t->parent_id = -1;
    t->lineage_id = -1;
    t->caps = 0;
    scheduler_reset_file_descriptors_to_std(t);
    t->env_block = NULL;
    t->cmdline_block = NULL;
    t->cmdline_length = 0;
    t->env_length = 0;
    t->env_count = 0;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_restorer = 0;
    t->sig_siginfo = 0;
    t->sig_resethand = 0;
    t->sig_onstack = 0;
    t->sig_alt_stack_base = 0;
    t->sig_alt_stack_size = 0;
    t->sig_on_alt_stack = 0;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_address = 0;
    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = SIG_DFL_ADDR;
    }
    scheduler_regions_forget_memfds(t);
    scheduler_regions_release(t);
    t->fs_base = 0;
    t->is_thread = 0;
    t->detached = 0;
    t->exiting = 0;
    t->ending = 0;
    t->tgid = 0;
    t->kernel_task = 0;
    t->group_peak = 0;
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    /* Already released above. The next task in this slot allocates its own. */
    t->descriptor_table = (file_descriptor_table_t *)0;
    set_task_name(t, "");
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    if (stack) {
        physical_memory_free_contiguous((uint64_t)(uintptr_t)stack, TASK_STACK_SIZE / 4096);
    }

    /* And the slot held above, now that the thread using the address space
       has gone. A leader is never a thread, so this recurses exactly once.

       This is here rather than in task_exit_with_code because that runs from
       the scheduler - a fatal signal is delivered on the timer interrupt -
       and this function waits for a task to leave its kernel stack and takes
       the scheduler lock. Reaping is work for whoever called wait(). */
    if (reaped_a_thread &&
        !thread_group_has_live_members(reaped_group, (const task_t *)0)) {
        task_t *leader = scheduler_task_by_id(reaped_group);
        if (leader && leader != t && leader->state == TASK_TERMINATED &&
            !leader->is_thread && nobody_will_wait(leader)) {
            scheduler_reap_slot(leader);
        }
    }
}

static int wake_task_locked(task_t *t);

void scheduler_wake_task(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    int woken = wake_task_locked(t);
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
}

uint32_t scheduler_cpus_holding_address_space(uint64_t pml4_phys) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t mask = 0;
    for (int c = 0; c < MAX_CPUS; c++) {
        if (__atomic_load_n(&loaded_pml4_phys[c], __ATOMIC_SEQ_CST) == pml4_phys) {
            mask |= 1u << c;
        }
    }
    return mask;
}

/* M204: the one way to put a different address space on this core outside
   schedule(). loaded_pml4_phys is what every TLB shootdown asks "who holds
   this address space", so it is published before the load, as schedule()
   does. execve loaded the new program's tables and published nothing: its
   core went on recorded as holding the tables it had just destroyed, and as
   NOT holding the new ones - so until the new program's first thread was
   switched out, a shootdown for its address space skipped the core it ran
   on. A renderer's allocator decommits from one thread while its main thread
   runs, and the main thread went on writing through translations of frames
   that had been freed and handed to somebody else: PartitionAlloc's freelist
   in the renderer, and the browser's heap when the frame went there, both
   found corrupt within half a second of a renderer starting. */
void scheduler_load_address_space(task_t *t, uint64_t pml4_phys) {
    uint64_t flags = irq_save_disable();
    t->pml4_phys = pml4_phys;
    __atomic_store_n(&loaded_pml4_phys[smp_current_cpu()], pml4_phys, __ATOMIC_SEQ_CST);
    virtual_memory_switch_address_space(pml4_phys);
    irq_restore(flags);
}

void scheduler_forget_address_space(uint64_t pml4_phys) {
    for (int c = 0; c < MAX_CPUS; c++) {
        if (loaded_pml4_phys[c] == pml4_phys) {
            loaded_pml4_phys[c] = 0;
        }
    }
}

/* Whether any OTHER live task shares this address space: the question a
   copy-on-write break has to answer before deciding whether the other cores
   need telling. */
int scheduler_address_space_is_shared(task_t *owner) {
    if (!owner) {
        return 0;
    }
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        if (t != owner && t->state != TASK_FREE && t->state != TASK_TERMINATED &&
            t->pml4_phys == owner->pml4_phys) {
            return 1;
        }
    }
    return 0;
}

task_t *scheduler_vm_owner(task_t *t) {
    if (!t || !t->is_thread) {
        return t;
    }
    task_t *leader = scheduler_task_by_id(t->tgid);
    return leader ? leader : t;
}

#define FILL_REFUSE ((uint64_t)-1)

static uint64_t fill_policy(uint64_t page, int for_write, int for_exec,
                            const mmap_region_t *region) {
    if (page >= USER_STACK_LIMIT && page < USER_STACK_TOP) {
        if (for_exec) {
            return FILL_REFUSE;
        }
        return VIRTUAL_MEMORY_FLAG_USER | VIRTUAL_MEMORY_FLAG_WRITABLE;
    }

    if (page < USER_MMAP_BASE || page >= USER_MMAP_LIMIT) {
        return FILL_REFUSE;
    }
    /* M180: the region is handed in rather than looked up again. This used to
       be a second copy of mmap_region_for's walk, with the same fault of
       re-reading the table on every iteration - so M179 fixed one of the two
       places a page fault reads this table and left the other. One lookup per
       fault is also one ANSWER: the two walks could disagree if a writer ran
       between them, and the caller would then map a page with the permissions
       of a region that the code doing the mapping had not found. */
    if (!region) {
        return FILL_REFUSE;
    }
    if ((region->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) {
        return FILL_REFUSE;
    }
    if (for_write && !(region->prot & PROT_WRITE)) {
        return FILL_REFUSE;
    }
    if (for_exec && !(region->prot & PROT_EXEC)) {
        return FILL_REFUSE;
    }
    uint64_t flags = VIRTUAL_MEMORY_FLAG_USER;
    if (region->prot & PROT_WRITE) {
        flags |= VIRTUAL_MEMORY_FLAG_WRITABLE;
    }
    if (region->prot & PROT_EXEC) {
        flags |= VIRTUAL_MEMORY_FLAG_EXEC;
    }
    return flags;
}

#ifdef LEANOS_HOST_TEST
/* M225: where a host test stands in for a writer on another core - between
   two entries of a walk, which is the only place the re-check after an
   unlocked walk has anything to catch (tests/fakes/fake_kernel_objects.c). */
void fake_region_walk_step(void *task, uint32_t index);
#define REGION_WALK_STEP(self, i) fake_region_walk_step((self), (i))
#else
#define REGION_WALK_STEP(self, i) ((void)0)
#endif

static const mmap_region_t *mmap_region_for(task_t *self, uint64_t page) {
    if (page < USER_MMAP_BASE || page >= USER_MMAP_LIMIT) {
        return 0;
    }
    /* M179: capacity first, then the pointer, and then neither again - see
       scheduler_regions_reserve for why that order and not the other. Reading
       self->mmaps once per iteration was the other half of the same bug: the
       table can be replaced between two iterations of a loop that is walking
       it, which no amount of bounds checking helps with. */
    uint32_t capacity = __atomic_load_n(&self->mmap_capacity, __ATOMIC_ACQUIRE);
    const mmap_region_t *table = __atomic_load_n(&self->mmaps, __ATOMIC_ACQUIRE);
    if (!table) {
        return 0;
    }
    for (uint32_t i = 0; i < capacity; i++) {
        if (table[i].pages == 0) {
            break;
        }
        uint64_t start = table[i].base;
        uint64_t end = start + (uint64_t)table[i].pages * PAGE_SIZE;
        if (page >= start && page < end) {
            return &table[i];
        }
        REGION_WALK_STEP(self, i);
    }
    return 0;
}

/* M180: the one way the fault path asks what covers an address, and it hands
   back a COPY rather than a pointer into the table. A pointer stays correct
   only as long as nobody shifts the entries around it, and sys_munmap does
   exactly that while it works - so a caller holding one has a window it cannot
   see. Everything the caller then does with the answer - reading a file,
   mapping a frame, taking a memfd reference - is far too slow to hold the
   table still across in any case.

   A copy can be stale the moment it is taken. That is the same staleness a
   caller racing a writer would have had anyway; what it cannot be is torn, or
   half of one region and half of another. */
/* M181: the fault path does NOT take mmap_lock, and that is a measurement
   rather than an oversight. Taking it there cost six four-core browser runs
   out of six - Chromium never drew at all, and one boot did not even reach
   the browser. Every page fault would contend for a per-process lock, and a
   spinlock under TCG is worse than it looks: a core that spins still burns
   its whole slice of the one host thread the emulator gives it, so the cores
   waiting starve the core holding it. On one core the same kernel is
   unchanged - the graded battery took 344 s either way - which is what says
   it is contention and not cost.

   What the reader relies on instead is M179 and M180: the table is never
   freed while somebody might be walking it, the capacity is loaded before the
   pointer so the pair cannot tear the dangerous way, and this returns a COPY
   so nothing is read twice. The hole left is an entry read while a writer is
   part way through shifting it, which is narrower than what was there before
   and is what a seqlock would close without a reader ever blocking.

   M225: and that hole was open in more shapes than one entry being moved.
   A split (mprotect, munmap of a middle) shrinks a region and only then
   inserts its tail; an unmap shifts every entry above it DOWN while the
   reader walks UP, so the reader can step over the one it wanted; a merge
   moves a region's base before it adds to its length. In each, the unlocked
   walk finds nothing for a page that is mapped, fill_policy refuses, and the
   thread is killed for touching memory mmap had just given it - threadtest's
   own TLS block, in two new threads at once, on four cores under TCG.

   So the unlocked look is now the FIRST look. Writers bump mmap_sequence
   when they start and when they finish (scheduler_regions_begin_change);
   if it was odd before this look, or is different after it, a writer was in
   the table while it was read, and the answer is asked again under the lock,
   where it cannot be half of anything. A fault that meets no writer - all
   but a handful - still takes no lock, which is what M181 measured. */
static int mmap_region_snapshot_at(task_t *self, uint64_t page, mmap_region_t *out,
                                   uint32_t *sequence_out) {
    uint32_t before = __atomic_load_n(&self->mmap_sequence, __ATOMIC_ACQUIRE);
    if ((before & 1u) == 0) {
        mmap_region_t copy = {0};
        const mmap_region_t *found = mmap_region_for(self, page);
        if (found) {
            copy = *found;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&self->mmap_sequence, __ATOMIC_RELAXED) == before) {
            if (found) {
                *out = copy;
            }
            *sequence_out = before;
            return found != 0;
        }
    }
    uint64_t flags = spin_lock_irqsave(&self->mmap_lock);
    const mmap_region_t *found = mmap_region_for(self, page);
    if (found) {
        *out = *found;
    }
    /* Even here: a writer holds this lock for as long as it is odd. */
    *sequence_out = __atomic_load_n(&self->mmap_sequence, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&self->mmap_lock, flags);
    return found != 0;
}

static int mmap_region_snapshot(task_t *self, uint64_t page, mmap_region_t *out) {
    uint32_t ignored;
    return mmap_region_snapshot_at(self, page, out, &ignored);
}

uint64_t scheduler_regions_lock(task_t *t) {
    return spin_lock_irqsave(&t->mmap_lock);
}

void scheduler_regions_unlock(task_t *t, uint64_t flags) {
    spin_unlock_irqrestore(&t->mmap_lock, flags);
}

/* The two bumps are full barriers (a locked add on this machine), so no store
   to the table can be seen before the first or after the second. */
uint64_t scheduler_regions_begin_change(task_t *t) {
    uint64_t flags = spin_lock_irqsave(&t->mmap_lock);
    __atomic_add_fetch(&t->mmap_sequence, 1, __ATOMIC_SEQ_CST);
    return flags;
}

void scheduler_regions_end_change(task_t *t, uint64_t flags) {
    __atomic_add_fetch(&t->mmap_sequence, 1, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&t->mmap_lock, flags);
}

int scheduler_region_covers(task_t *t, uint64_t page) {
    mmap_region_t ignored;
    return mmap_region_snapshot(t, page, &ignored);
}

void scheduler_region_forget_memfd(mmap_region_t *r) {
    if (!r || !r->memfd_id) {
        return;
    }
    struct memfd *m = memfd_by_tag((uint32_t)(r->memfd_id - 1), r->memfd_gen);
    r->memfd_id = 0;
    r->memfd_gen = 0;
    memfd_region_unref(m);
}

/* The table's lifetime lives here rather than beside the mmap syscalls,
   because a task is what owns it and a task is what this file makes and
   unmakes. */
int scheduler_regions_reserve(task_t *t) {
    uint32_t used = 0;
    while (used < t->mmap_capacity && t->mmaps[used].pages != 0) {
        used++;
    }
    if (used + 1 < t->mmap_capacity) {
        return 0;
    }
    /* One spare entry is kept past the last region in use, because the array
       is terminated by a zero `pages` and every reader stops there. */
    uint32_t wanted = t->mmap_capacity ? t->mmap_capacity * 2
                                       : MMAP_REGIONS_INITIAL;
    if (wanted > MAX_MMAP_REGIONS) {
        wanted = MAX_MMAP_REGIONS;
    }
    if (wanted <= t->mmap_capacity) {
        return -1;
    }
    mmap_region_t *grown = (mmap_region_t *)kmalloc(sizeof(mmap_region_t) *
                                                    (size_t)wanted);
    if (!grown) {
        return -1;
    }
    for (uint32_t i = 0; i < wanted; i++) {
        grown[i].base = 0;
        grown[i].pages = 0;
        grown[i].prot = 0;
        grown[i].handle = -1;
        grown[i].file_page = 0;
        grown[i].shared = 0;
        grown[i].memfd_id = 0;
        grown[i].memfd_gen = 0;
    }
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
        grown[i] = t->mmaps[i];
    }
    /* M179: publish the POINTER first and the capacity second, and read them
       back the other way round in mmap_region_for. The two are not one word
       and a fault handler reads them without a lock, so the pair can be torn;
       of the two ways to tear it only one is dangerous. A reader that takes
       the new pointer with the old capacity walks a prefix of a table that is
       a superset of the old one, which is right. A reader that takes the OLD
       pointer with the new capacity walks off the end of it. Storing the
       pointer before the capacity, and loading the capacity before the
       pointer, is what makes the second one impossible. */
    mmap_region_t *old = t->mmaps;
    __atomic_store_n(&t->mmaps, grown, __ATOMIC_RELEASE);
    __atomic_store_n(&t->mmap_capacity, wanted, __ATOMIC_RELEASE);
    /* And the old table is kept rather than freed - see mmaps_retired. */
    if (old) {
        if (t->mmaps_retired_count >= MMAP_RETIRED_MAX) {
            panic("sched: more mmap tables retired than the capacity can double");
        }
        t->mmaps_retired[t->mmaps_retired_count++] = old;
    }
    return 0;
}

void scheduler_regions_release(task_t *t) {
    mmap_region_t *table = t->mmaps;
    t->mmaps = 0;
    t->mmap_capacity = 0;
    kfree(table);
    /* M179: and everything this table grew out of, which nothing can be
       reading any more because the address space itself is going. */
    for (uint32_t i = 0; i < t->mmaps_retired_count; i++) {
        kfree(t->mmaps_retired[i]);
        t->mmaps_retired[i] = 0;
    }
    t->mmaps_retired_count = 0;
}

void scheduler_regions_forget_memfds(task_t *t) {
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
        scheduler_region_forget_memfd(&t->mmaps[i]);
    }
}

void scheduler_regions_retain_memfds(task_t *t) {
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
        if (t->mmaps[i].pages == 0 || !t->mmaps[i].memfd_id) {
            continue;
        }
        struct memfd *m = memfd_by_tag((uint32_t)(t->mmaps[i].memfd_id - 1),
                                       t->mmaps[i].memfd_gen);
        if (!m) {
            t->mmaps[i].memfd_id = 0;
            t->mmaps[i].memfd_gen = 0;
            continue;
        }
        memfd_region_reference(m);
    }
}

static int region_is_shared(const mmap_region_t *r) {
    return r->memfd_id != 0 || (r->shared && r->handle >= 0);
}

/* Taken under mmap_lock so it is a list of whole regions rather than of
   half-shifted ones, and the heap is not touched with the lock held: count,
   allocate, fill, and start again if a region appeared in between. */
int scheduler_shared_ranges(task_t *t, virtual_memory_range_t **out) {
    *out = (virtual_memory_range_t *)0;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&t->mmap_lock);
        int count = 0;
        for (uint32_t i = 0; i < t->mmap_capacity; i++) {
            if (t->mmaps[i].pages == 0) {
                break;
            }
            if (region_is_shared(&t->mmaps[i])) {
                count++;
            }
        }
        spin_unlock_irqrestore(&t->mmap_lock, flags);
        if (count == 0) {
            return 0;
        }

        virtual_memory_range_t *ranges =
            (virtual_memory_range_t *)kmalloc(sizeof(virtual_memory_range_t) * (size_t)count);
        if (!ranges) {
            return -1;
        }
        int n = 0;
        int fits = 1;
        flags = spin_lock_irqsave(&t->mmap_lock);
        for (uint32_t i = 0; i < t->mmap_capacity; i++) {
            if (t->mmaps[i].pages == 0) {
                break;
            }
            if (!region_is_shared(&t->mmaps[i])) {
                continue;
            }
            if (n == count) {
                fits = 0;
                break;
            }
            ranges[n].lo = t->mmaps[i].base;
            ranges[n].hi = t->mmaps[i].base + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
            n++;
        }
        spin_unlock_irqrestore(&t->mmap_lock, flags);
        if (!fits) {
            kfree(ranges);
            continue;
        }
        for (int i = 1; i < n; i++) {
            virtual_memory_range_t key = ranges[i];
            int j = i - 1;
            while (j >= 0 && ranges[j].lo > key.lo) {
                ranges[j + 1] = ranges[j];
                j--;
            }
            ranges[j + 1] = key;
        }
        *out = ranges;
        return n;
    }
}

int scheduler_release_shared_range(task_t *t, uint64_t start, uint64_t end) {
    int dropped = 0;
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
        }
        if (t->mmaps[i].memfd_id) {
            uint64_t rstart = t->mmaps[i].base;
            uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
            uint64_t from = start > rstart ? start : rstart;
            uint64_t to = end < rend ? end : rend;
            int here = 0;
            for (uint64_t p = from; p < to; p += PAGE_SIZE) {
                if (!virtual_memory_user_range_ok(t->pml4_phys, p, 1, 0)) {
                    continue;
                }
                virtual_memory_unmap_page_in(t->pml4_phys, p);
                here++;
            }
            if (here) {
                virtual_memory_flush_other_cpus(t->pml4_phys);
            }
            dropped += here;
            continue;
        }
        if (!t->mmaps[i].shared || t->mmaps[i].handle < 0) {
            continue;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        uint64_t from = start > rstart ? start : rstart;
        uint64_t to = end < rend ? end : rend;
        /* The put can free the frame, so it waits until no other core can
           still reach that frame through a translation of this page. */
        uint32_t pending[32];
        int count = 0;
        for (uint64_t p = from; p < to; p += PAGE_SIZE) {
            if (!virtual_memory_user_range_ok(t->pml4_phys, p, 1, 0)) {
                continue;
            }
            virtual_memory_unmap_page_in(t->pml4_phys, p);
            pending[count++] = t->mmaps[i].file_page + (uint32_t)((p - rstart) / PAGE_SIZE);
            dropped++;
            if (count == (int)(sizeof(pending) / sizeof(pending[0]))) {
                virtual_memory_flush_other_cpus(t->pml4_phys);
                for (int k = 0; k < count; k++) {
                    file_mapping_put(t->mmaps[i].handle, pending[k]);
                }
                count = 0;
            }
        }
        if (count) {
            virtual_memory_flush_other_cpus(t->pml4_phys);
            for (int k = 0; k < count; k++) {
                file_mapping_put(t->mmaps[i].handle, pending[k]);
            }
        }
    }
    return dropped;
}

/* What a page of a region is backed by, for telling whether a region found
   later is still the one a page was filled from: the same memfd page, the
   same page of the same shared file, the same page of a private file mapping
   (its contents were read from there), or anonymous memory. */
static int same_backing(const mmap_region_t *a, const mmap_region_t *b, uint64_t page) {
    if (a->memfd_id != b->memfd_id || a->memfd_gen != b->memfd_gen ||
        a->handle != b->handle || a->shared != b->shared) {
        return 0;
    }
    if (a->memfd_id == 0 && a->handle < 0) {
        return 1;
    }
    uint32_t ia = a->file_page + (uint32_t)((page - a->base) / PAGE_SIZE);
    uint32_t ib = b->file_page + (uint32_t)((page - b->base) / PAGE_SIZE);
    return ia == ib;
}

/* M225: the other half of the region table's seqlock, after the page is in.

   The snapshot a fault maps from can be stale by the time it maps: nothing
   holds the table still between the look and the map, on purpose (M181). A
   writer that ran in that gap did its work on the pages that were present -
   munmap released them, mprotect changed their permissions - and this one
   was not present yet. So it went in afterwards, from a region the table no
   longer had: a memfd frame mapped at an address with no region, which
   nothing would ever unmap - the memfd frees it when it is closed while it
   is still mapped here, or the teardown frees it as the address space's own
   and the memfd frees it again (the M173 double free) - or a page writable
   after mprotect(PROT_READ) had returned.

   The sequence counter says whether that can have happened. Read again after
   the page is in (fenced, against the writer's locked bump), unchanged means
   no writer started before the page was visible - and one that starts later
   finds it present and deals with it. Changed, the question is asked again
   under the writers' lock, and the page is given what the writers would
   have given it had it been present: taken back out if the region it was
   filled from is gone or no longer this page's, its permissions made the
   region's if only those changed. Only the frame this fault put there is
   touched - anything else at the address got there by somebody else's
   right - and the access is retried either way, so a page that is really
   gone faults again and is refused by a snapshot that is not stale. */
static void revalidate_filled_page(task_t *self, uint64_t page, const mmap_region_t *filled_from,
                                   uint32_t sequence, uint64_t phys, uint64_t flags) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&self->mmap_sequence, __ATOMIC_RELAXED) == sequence) {
        return;
    }
    uint64_t lock_flags = spin_lock_irqsave(&self->mmap_lock);
    const mmap_region_t *now = mmap_region_for(self, page);
    uint64_t entry = virtual_memory_lookup_frame(self->pml4_phys, page);
    if (!(entry & 1u) || (entry & 0x000FFFFFFFFFF000ULL) != phys) {
        spin_unlock_irqrestore(&self->mmap_lock, lock_flags);
        return;
    }
    if (!now || !same_backing(now, filled_from, page)) {
        uint64_t taken = virtual_memory_unmap_page_take(self->pml4_phys, page);
        if (taken) {
            virtual_memory_flush_other_cpus(self->pml4_phys);
            if (filled_from->memfd_id) {
                /* The memfd's own frame: the mapping held no reference. */
            } else if (filled_from->handle >= 0 && filled_from->shared) {
                file_mapping_put(filled_from->handle,
                                 filled_from->file_page +
                                     (uint32_t)((page - filled_from->base) / PAGE_SIZE));
            } else {
                physical_memory_free_frame(taken);
            }
        }
    } else {
        uint64_t want = fill_policy(page, 0, 0, now);
        if (want == FILL_REFUSE) {
            want = VIRTUAL_MEMORY_FLAG_USER;
        }
        if (want != flags) {
            /* What sys_mprotect does to a page that is present. */
            virtual_memory_protect_range_in(self->pml4_phys, page, page + PAGE_SIZE, want);
        }
    }
    spin_unlock_irqrestore(&self->mmap_lock, lock_flags);
}

static int fill_one_page_ex(task_t *self, uint64_t page, int for_write, int for_exec) {

    /* Present already: a sibling thread faulted on the same page and filled
       it first. This fault was real when it was taken, so the access is
       retried rather than refused - answering 0 here turned the loser of
       that race into a SIGSEGV, SEGV_ACCERR, on a page it had every right to
       write (M196: Chromium's histogram allocator, on eight real cores). If
       the retry really is not allowed it faults again, as a protection fault,
       and is refused there. */
    if (virtual_memory_user_range_ok(self->pml4_phys, page, 1, 0)) {
        return 1;
    }

    /* M180: one snapshot, used by the policy and by everything below it. */
    mmap_region_t snapshot;
    uint32_t sequence = 0;
    const mmap_region_t *region = mmap_region_snapshot_at(self, page, &snapshot, &sequence)
                                      ? &snapshot
                                      : (const mmap_region_t *)0;

    uint64_t flags = fill_policy(page, for_write, for_exec, region);
    if (flags == FILL_REFUSE) {
        return 0;
    }

    if (region && region->memfd_id) {
        struct memfd *m = memfd_by_tag((uint32_t)(region->memfd_id - 1), region->memfd_gen);
        if (!m) {
            return 0;
        }
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        uint64_t phys = memfd_frame(m, index);
        if (phys == 0) {
            return 0;
        }
        int installed = virtual_memory_try_map_page_if_absent(self->pml4_phys, page, phys, flags);
        if (installed == 0) {
            revalidate_filled_page(self, page, region, sequence, phys, flags);
        }
        return installed >= 0 ? 1 : FILL_NO_MEMORY;
    }
    if (region && region->handle >= 0 && region->shared) {
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        uint64_t sphys = file_mapping_get(region->handle, index,
                                     (region->prot & PROT_WRITE) != 0);
        if (sphys == 0) {
            return FILL_NO_MEMORY;
        }
        int installed = virtual_memory_try_map_page_if_absent(self->pml4_phys, page, sphys, flags);
        if (installed != 0) {
            file_mapping_put(region->handle, index);
        } else {
            revalidate_filled_page(self, page, region, sequence, sphys, flags);
        }
        return installed < 0 ? FILL_NO_MEMORY : 1;
    }

    uint64_t phys = physical_memory_try_alloc_frame();
    if (phys == 0) {
        return FILL_NO_MEMORY;
    }
    k_memset((void *)phys, 0, PAGE_SIZE);
    if (region && region->handle >= 0) {
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        (void)virtual_file_system_handle_read(region->handle, (void *)phys, PAGE_SIZE,
                              index * PAGE_SIZE);
    }
    int installed = virtual_memory_try_map_page_if_absent(self->pml4_phys, page, phys, flags);
    if (installed != 0) {
        physical_memory_free_frame(phys);
    } else if (region) {
        revalidate_filled_page(self, page, region, sequence, phys, flags);
    }
    return installed < 0 ? FILL_NO_MEMORY : 1;
}

static int fill_one_page(task_t *self, uint64_t page, int for_write) {
    return fill_one_page_ex(self, page, for_write, 0);
}

#define STACK_GROW_SLACK 65536ULL

/* Whether this address is inside something this process asked for, which is
   a different question from whether a page is present at it. Demand paging
   makes the hardware's present bit useless for telling SEGV_MAPERR from
   SEGV_ACCERR: a write to a PROT_READ page that has never been touched
   arrives with the bit clear, and answering MAPERR there would say there was
   no mapping when there was one and the access was refused. */
int scheduler_address_is_mapped(uint64_t address) {
    task_t *self = scheduler_vm_owner(scheduler_current());
    if (!self || self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return 0;
    }
    uint64_t page = address & ~(uint64_t)(PAGE_SIZE - 1);
    if (virtual_memory_user_range_ok(self->pml4_phys, page, 1, 0)) {
        return 1;
    }
    mmap_region_t probe;
    if (mmap_region_snapshot(self, page, &probe)) {
        return 1;
    }
    /* The stack grows into its own area on demand, so an address inside it
       and above the stack pointer is mapped in every sense that matters. */
    if (page >= USER_STACK_LIMIT && page < USER_STACK_TOP) {
        return 1;
    }
    return 0;
}

int scheduler_fault_fill(uint64_t address, uint64_t error_code, uint64_t user_rsp) {
    task_t *self = scheduler_vm_owner(scheduler_current());
    if (!self || self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return 0;
    }
    uint64_t page = address & ~(uint64_t)(PAGE_SIZE - 1);

    if (page >= USER_STACK_LIMIT && page < USER_STACK_TOP) {
        if (address + STACK_GROW_SLACK < user_rsp) {
            return 0;
        }
    }

    if (error_code & 1u) {
        if (error_code & 2u) {
            int broke = virtual_memory_cow_break(self->pml4_phys, page);
            return broke ? 1 : 0;
        }
        return 0;
    }
    return fill_one_page_ex(self, page, (error_code & 2u) != 0, (error_code & 16u) != 0);
}

void scheduler_prefault_range(uint64_t address, uint64_t length, int for_write) {
    if (length == 0) {
        return;
    }
    task_t *self = scheduler_vm_owner(scheduler_current());
    if (!self || self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return;
    }
    if (virtual_memory_user_range_ok(self->pml4_phys, address, length, for_write)) {
        return;
    }
    uint64_t first = address & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (address + length - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (last < first) {
        return;
    }
    int in_arena = !(last < USER_MMAP_BASE || first >= USER_MMAP_LIMIT);
    int in_stack = !(last < USER_STACK_LIMIT || first >= USER_STACK_TOP);
    int in_image = !(last < USER_IMAGE_BASE || first >= USER_IMAGE_LIMIT);
    /* M197. A process that has forked has every page of its own data
       segment copy-on-write, and the kernel writing a result into one of
       them - a pipe's two descriptors, a stat, a read - asked only whether
       the page was writable, found it was not, and refused. The program's
       own store to the same byte would have faulted and been given the page.
       Chromium's browser forks its children and then opens the pipe it tells
       the compositor it has drawn through; the open failed on a static in
       its data segment, every frame it presented after that went nowhere,
       and the desktop's once-a-second full redraw was all that ever put the
       browser on the screen. The image is filled at exec and never demand
       paged, so all a write here can need is the copy - and a page that is
       read-only for real, text, is not copy-on-write and stays refused. */
    if (in_image && !in_arena && !in_stack) {
        if (for_write) {
            for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
                if (!virtual_memory_user_range_ok(self->pml4_phys, page, 1, 1)) {
                    virtual_memory_cow_break(self->pml4_phys, page);
                }
            }
        }
        return;
    }
    if (!in_arena && !in_stack) {
        return;
    }
    for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
        if (virtual_memory_user_range_ok(self->pml4_phys, page, 1, for_write)) {
            continue;
        }
        if (for_write) {
            int broke = virtual_memory_cow_break(self->pml4_phys, page);
            if (broke) {
                continue;
            }
        }
        fill_one_page(self, page, for_write);
    }
}

static char *duplicate_record(task_t *t, int env, uint32_t *length_out, uint32_t *count_out,
                              int *no_memory);

task_t *task_fork(uint64_t child_pml4, const isr_regs_t *regs) {
    _Static_assert(TASK_STACK_SIZE % 4096 == 0, "a kernel stack must be a whole number of frames");
    uint8_t *stack_base = (uint8_t *)(uintptr_t)physical_memory_try_alloc_contiguous_anywhere(TASK_STACK_SIZE / 4096);
    if (!stack_base) {
        return (task_t *)0;
    }

    file_descriptor_table_t *fresh = descriptor_table_new();
    if (!fresh) {
        physical_memory_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
        return (task_t *)0;
    }

    /* The child's region table is allocated BEFORE the scheduler lock,
       because kmalloc with interrupts off and a spinlock held is a different
       promise from the one this allocator makes. The parent is the caller, so
       its table cannot change size underneath this. */
    task_t *forking = scheduler_vm_owner(scheduler_current());
    uint32_t child_capacity = forking ? forking->mmap_capacity : 0;
    mmap_region_t *child_regions = 0;
    if (child_capacity) {
        child_regions = (mmap_region_t *)kmalloc(sizeof(mmap_region_t) *
                                                 (size_t)child_capacity);
        if (!child_regions) {
            physical_memory_free_contiguous((uint64_t)(uintptr_t)stack_base,
                                            TASK_STACK_SIZE / 4096);
            descriptor_table_release(fresh);
            return (task_t *)0;
        }
        /* M181: the parent's table is copied HERE, before scheduler_lock, and
           the two locks are never held together.

           Holding scheduler_lock while waiting for this one was the first
           version and it stalled the machine: a fork would sit on the lock
           every CPU needs to switch tasks while waiting for a lock any thread
           calling mmap can hold, so one writer anywhere stopped the scheduler
           everywhere. Four-core boots hung at [m43] and three attempts of
           fork-smp-test.sh ran to their ceiling without reaching the test.

           Nothing can see the child's table yet - the child has no slot - so
           only the parent's side needs holding still. */
        uint64_t parent_region_flags = spin_lock_irqsave(&forking->mmap_lock);
        for (uint32_t i = 0; i < child_capacity; i++) {
            child_regions[i] = forking->mmaps[i];
        }
        spin_unlock_irqrestore(&forking->mmap_lock, parent_region_flags);
    }

    /* M204: and the descriptors are copied here too, for the same reason.
       Retaining a pipe end takes pipe_lock, and a write into a full pipe
       holds pipe_lock while it wakes the reader and blocks - both of which
       take scheduler_lock. A fork retaining under scheduler_lock while a
       writer filled a pipe was the two locks taken in opposite orders, and
       the machine stopped: every core that then needed either lock spun for
       good. A browser forks while the desktop's pipes fill; it took a
       profiler pausing the machine at awkward moments to make it happen on
       purpose.

       M225: "the table is the caller's own, so it is the caller's to read"
       was true of the table and not of its slots: the caller's sibling
       threads share it, and one closing a descriptor while this copied it
       could drop the object's last reference between the copy and the
       child's reference - a reference taken on something already gone. The
       copy is made under the table's lock now (descriptor_table_copy). */
    task_t *caller = scheduler_current();
    descriptor_table_copy(fresh, caller->descriptor_table, 0, MAX_FILE_DESCRIPTORS);

    /* M225: the child's command line and environment records are copied
       here too, before the lock and before the child exists. The command
       line was copied with kmalloc under scheduler_lock (the promise the
       region table above is careful not to ask of this allocator), and the
       environment was set by sys_fork AFTER this returned - into a child
       that could already have run on another processor, exec'd, failed,
       _exit(127)'d, been reaped by a sibling thread's waitpid(-1) and had
       its slot refilled. A copy that cannot be made fails the fork now,
       rather than SIGKILLing a child that is already somebody's. */
    char *cmdline_copy = (char *)0;
    uint32_t cmdline_length = 0;
    char *env_copy = (char *)0;
    uint32_t env_length = 0;
    uint32_t env_count = 0;
    /* M225 (process-lifetimes): copied out under the lock, the way procfs
       reads them, rather than read off `forking` with none - an exec on
       another thread of this process replaces a record while this runs. A
       command line that cannot be copied is left out, as it always was; an
       environment that cannot be fails the fork. */
    int no_memory = 0;
    if (forking) {
        int cmdline_no_memory = 0;
        cmdline_copy = duplicate_record(forking, 0, &cmdline_length, (uint32_t *)0,
                                        &cmdline_no_memory);
        (void)cmdline_no_memory;
        env_copy = duplicate_record(forking, 1, &env_length, &env_count, &no_memory);
    }
    if (no_memory) {
        if (cmdline_copy) {
            kfree(cmdline_copy);
        }
        physical_memory_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
        descriptor_table_release(fresh);
        kfree(child_regions);
        return (task_t *)0;
    }

    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);

    int slot = -1;
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (task_count >= MAX_TASKS) {
            spin_unlock(&scheduler_lock);
            irq_restore(flags);
            physical_memory_free_contiguous((uint64_t)(uintptr_t)stack_base, TASK_STACK_SIZE / 4096);
            descriptor_table_release(fresh);
            kfree(child_regions);
            if (cmdline_copy) {
                kfree(cmdline_copy);
            }
            if (env_copy) {
                kfree(env_copy);
            }
            return NULL;
        }
        slot = task_count++;
    }

    task_t *t = &tasks[slot];
    task_t *parent = current_task[smp_current_cpu()];

    /* Two different parents. The thread that called fork is what the child's
       registers, floating point state and thread pointer come from, because
       the child resumes where the caller was. Everything that describes the
       ADDRESS SPACE - the mappings, the break, where the next shared segment
       would go - belongs to whichever task owns it, and a thread's own copies
       of those are empty. Reading them off the caller gave a child forked
       from a thread no mappings and a break of zero. */
    task_t *space = scheduler_vm_owner(parent);

    t->id = PID_MAKE(slot, t->generation);
    t->entry = NULL;
    t->arg = NULL;
    t->state = TASK_READY;
    t->pml4_phys = child_pml4;
    t->stack_base = stack_base;
    t->kernel_stack_top = (uint64_t)(stack_base + TASK_STACK_SIZE);
    t->home_cpu = -1;
    t->kernel_activity = KERNEL_ACTIVITY_NONE;
    t->syscalls = 0;
    t->descriptor_uses = (file_descriptor_use_t *)0;
    t->stamp_syscalls_seen = 0;

    t->descriptor_table = fresh;
    t->parent_id = parent->tgid;
    t->lineage_id = parent->tgid;
    t->kernel_task = 0;
    t->pgid = parent->pgid;
    t->sid = parent->sid;
    t->pending_signal = 0;
    t->reaped = 0;
    t->exit_signal = 0;
    t->user_ticks = 0;
    t->sys_ticks = 0;
    t->child_user_ticks = 0;
    t->child_sys_ticks = 0;
    t->dead_thread_user_ticks = 0;
    t->dead_thread_sys_ticks = 0;
    t->max_rss_pages = 0;
    t->child_max_rss_pages = 0;

    for (int i = 0; i < PATH_MAX_LENGTH; i++) {
        t->cwd[i] = space->cwd[i];
        if (!space->cwd[i]) {
            break;
        }
    }
    if (!t->cwd[0]) {
        t->cwd[0] = '/';
        t->cwd[1] = '\0';
    }
    /* A fork has the command line of what it forked from until it execs, and
       a child caught between the two is a real thing to see in a process
       list: Chromium's launcher forks and then execs /proc/self/exe. Both
       records were copied above, before the lock. */
    t->cmdline_block = cmdline_copy;
    t->cmdline_length = cmdline_length;
    t->env_block = env_copy;
    t->env_length = env_length;
    t->env_count = env_count;

    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = parent->sig_handler[i];
    }
    t->sig_restorer = parent->sig_restorer;
    t->sig_blocked = parent->sig_blocked;
    t->sig_pending = 0;
    t->sig_siginfo = parent->sig_siginfo;
    t->sig_resethand = parent->sig_resethand;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_address = 0;

    /* The table allocated above, filled from the parent's. The slot the child
       is taking may have belonged to a task that mapped things, so whatever is
       there is given back first. */
    scheduler_regions_release(t);
    t->mmaps = child_regions;
    t->mmap_capacity = child_capacity;
    scheduler_regions_retain_memfds(t);

    t->tgid = t->id;
    t->is_thread = 0;
    t->group_peak = 1;
    t->spawned_id = 0;
    t->spawned_caps = 0;
    t->detached = 0;
    t->exiting = 0;
    t->ending = 0;
    t->caps = parent->caps;

    for (size_t i = 0; i < sizeof(t->fpu_state); i++) {
        t->fpu_state[i] = parent->fpu_state[i];
    }

    t->fs_base = parent->fs_base;

    t->heap_brk = space->heap_brk;
    t->heap_mapped_end = space->heap_mapped_end;
    t->shared_memory_next_vaddr = space->shared_memory_next_vaddr;
    set_task_name(t, parent->name);
    copy_task_name(t->program, parent->program);

    isr_regs_t *child_frame =
        (isr_regs_t *)(t->kernel_stack_top - sizeof(isr_regs_t));
    *child_frame = *regs;
    child_frame->rax = 0;

    uint64_t *sp = (uint64_t *)child_frame;
    *(--sp) = (uint64_t)fork_child_trampoline;
    *(--sp) = 0x2;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    t->rsp = (uint64_t)sp;

    /* M225: what fork() returns, recorded on the forking thread while the
       slot cannot change (scheduler_last_spawn) - sys_fork read child->id
       after this returned, and by then the child may have been reaped and
       the slot be another process's. */
    parent->spawned_id = t->id;
    parent->spawned_caps = t->caps;

    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return t;
}

task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg) {
    if (!leader) {
        return (task_t *)0;
    }
    return task_spawn_common(name, leader->pml4_phys, entry, arg, 0, 0, leader,
                             (const task_spawn_setup_t *)0);
}

#ifdef LEANOS_HOST_TEST
/* M225 (process-lifetimes): a host test asking something AS a task - a thread
   is only made by a task running in its process (task_spawn_thread), and the
   fakes have no other way to be running one. Puts `t` in `cpu`'s seat with
   nothing else changed and returns who was there, for the test to put back
   at once. Not built into the kernel. */
task_t *scheduler_host_test_seat(int cpu, task_t *t);
task_t *scheduler_host_test_seat(int cpu, task_t *t) {
    task_t *was = current_task[cpu];
    current_task[cpu] = t;
    return was;
}
#endif

int scheduler_count_sharing_address_space(uint64_t pml4_phys) {
    int n = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_FREE || tasks[i].state == TASK_TERMINATED) {
            continue;
        }
        if (tasks[i].pml4_phys == pml4_phys) {
            n++;
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return n;
}

void scheduler_kill_thread_group(task_t *t) {
    if (!t) {
        return;
    }
    /* M225: found and signalled in one hold of the lock. The victims used to
       be collected under it and signalled after it was dropped, through the
       door that checks no id - so a detached thread that finished in between
       (it is still in the list until it is TERMINATED), was swept by a spawn
       on another processor and had its slot refilled handed this process's
       SIGKILL to the newcomer: a kernel thread, or somebody else's program.
       Every exit() and every fatal signal of a process with threads comes
       through here, a browser's with a hundred and fifty of them. */
    int group = t->tgid;
    int woken = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    for (int i = 0; i < task_count; i++) {
        task_t *o = &tasks[i];
        if (o == t || o->state == TASK_FREE || o->state == TASK_TERMINATED || o->is_idle) {
            continue;
        }
        if (o->tgid == group) {
            woken += raise_signal_locked(o, SIGKILL);
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
}

/* POSIX asks two different questions about processor time: how much this
   thread has used, and how much the whole process has. This kernel accounts a
   tick to whichever task was running, and a thread IS a task, so the second
   answer is a sum over the thread group. A thread that has already exited is
   not missing from it: sched_reap_slot rolls a dead thread's ticks into its
   parent, which is another task in the same group. */
void scheduler_thread_group_ticks(task_t *t, uint64_t *user_ticks_out, uint64_t *sys_ticks_out) {
    uint64_t user = 0;
    uint64_t sys = 0;
    if (t) {
        int group = t->tgid;
        uint64_t flags = irq_save_disable();
        spin_lock(&scheduler_lock);
        for (int i = 0; i < task_count; i++) {
            task_t *o = &tasks[i];
            /* TASK_TERMINATED is deliberately counted: such a task still holds
               its own ticks until it is reaped, and reaping zeroes them in the
               same step that adds them to the parent, so neither window can
               count them twice. */
            if (o->state == TASK_FREE || o->tgid != group) {
                continue;
            }
            user += o->user_ticks;
            sys += o->sys_ticks;
            if (o->id == group) {
                user += o->dead_thread_user_ticks;
                sys += o->dead_thread_sys_ticks;
            }
        }
        spin_unlock(&scheduler_lock);
        irq_restore(flags);
    }
    if (user_ticks_out) {
        *user_ticks_out = user;
    }
    if (sys_ticks_out) {
        *sys_ticks_out = sys;
    }
}

/* The state changes scheduler_wake_task and scheduler_resume_stopped make,
   for a caller that already holds scheduler_lock. */
static int wake_task_locked(task_t *t) {
    event_sequence++;
    if (t->state != TASK_BLOCKED) {
        return 0;
    }
    blocked_count--;
    t->state = TASK_READY;
    t->ready_since_ms = clock_monotonic_ms();
    t->prio = PRIO_INTERACTIVE;
    t->full_slices = 0;
    t->wait_chan = (const void *)0;
    t->wake_deadline_ms = 0;
    return 1;
}

static int resume_stopped_locked(task_t *t) {
    if (t->state != TASK_STOPPED) {
        return 0;
    }
    t->state = TASK_READY;
    t->ready_since_ms = clock_monotonic_ms();
    t->prio = PRIO_INTERACTIVE;
    t->full_slices = 0;
    t->wait_chan = (const void *)0;
    t->wake_deadline_ms = 0;
    t->stopped_sig = 0;
    t->stop_reported = 0;
    event_sequence++;
    return 1;
}

/* What a signal does to a task that is certainly the one it was sent to,
   under scheduler_lock. Returns how many tasks it made runnable. */
static int raise_signal_locked(task_t *t, int sig) {
    int woken = 0;
    if (t->ending) {
        return 0;
    }
    if (sig == SIGCONT) {
        t->pending_stop = 0;
        woken += resume_stopped_locked(t);
    }

    if (!SIG_IS_CATCHABLE(sig)) {
        if (sig == SIGSTOP) {
            t->pending_stop = sig;
            return woken + wake_task_locked(t);
        }
        t->pending_signal = sig;
        woken += resume_stopped_locked(t);
        return woken + wake_task_locked(t);
    }
    uint64_t h = t->sig_handler[sig];
    if (h == SIG_IGN_ADDR) {
        return woken;
    }
    if (h == SIG_DFL_ADDR) {
        switch (SIG_DEFAULT_ACTION(sig)) {
        case SIG_DFL_IGNORE:
        case SIG_DFL_CONTINUE:
            return woken;
        case SIG_DFL_STOP:
            t->pending_stop = sig;
            return woken + wake_task_locked(t);
        default:
            break;
        }
        t->pending_signal = sig;
        woken += resume_stopped_locked(t);
        return woken + wake_task_locked(t);
    }
    t->sig_pending |= (1u << sig);
    return woken + wake_task_locked(t);
}

/* M225: the one door every signal goes through, and the check that the task
   is still the one it was sent to is made on the far side of it - under the
   scheduler lock, which is what a slot is filled under. kill(2) and the
   terminal find their targets in the task table without that lock, so the
   task they found can end, be swept and have its slot refilled - by a kernel
   thread, or by somebody the caller may not signal - before they raise. Both
   now hand over the id they checked (`expected_id`; 0 for a caller that holds
   the task itself, such as a fault or a task signalling itself) and whether
   the sender is the kernel, and a slot that is no longer that id, or is the
   kernel's when the sender is not, is not signalled. Returns whether the
   signal reached its task (an ignored signal has reached it). */
/* Under scheduler_lock: a live task of process `group` to take a signal sent
   to the process - one that is not on its way out and does not block it, if
   there is one, as Linux chooses. */
static task_t *live_member_for_signal_locked(int group, int sig) {
    task_t *fallback = (task_t *)0;
    for (int i = 0; i < task_count; i++) {
        task_t *o = &tasks[i];
        if (o->state == TASK_FREE || o->state == TASK_TERMINATED || o->is_idle ||
            o->tgid != group) {
            continue;
        }
        int blocked = sig > 0 && sig <= SIG_MAX && SIG_IS_CATCHABLE(sig) &&
                      (o->sig_blocked & (1u << sig)) != 0;
        if (!o->ending && !blocked) {
            return o;
        }
        if (!fallback) {
            fallback = o;
        }
    }
    return fallback;
}

/* Under scheduler_lock: the task a signal sent to `t` lands on, or 0.

   M225: an idle task is not a process and nothing may end it. It is the
   only way out of pick_next for a task that has terminated (M178), so
   a CPU whose idle tasks have all taken a fatal signal turns the next
   exit on it into "task_exit: terminated task resumed". kill(0, SIGKILL)
   from anything in process group 0 - which was everything the kernel
   starts, and the kernel itself - reached all sixteen of them, and the
   battery's M105 stage did exactly that by accident: it lost a writer to
   the spawn sweep, read a pid of 0 out of a null task and killed its own
   group with it. The idle tasks went one tick at a time, and the last of
   them panicked.

   M225: a pid names a PROCESS, and a process whose main thread has left
   through pthread_exit is still running in its other threads - wait()
   says so since M225 (scheduler_process_has_ended). kill(pid) refused it
   as ESRCH all the same, so a parent saw a child it could neither signal
   nor stop waiting for: the compositor's SIGTERM-then-SIGKILL to a client,
   the Task Manager's End, Node's subprocess.kill. Linux delivers to the
   thread group. So does this, when the sender named the leader by its id:
   the signal goes to a live thread of the process. A sender holding the
   task itself (expected_id 0) meant that task, and a thread's id is that
   thread's. */
static task_t *signal_target_locked(task_t *t, int expected_id, int sig, int from_kernel) {
    if (t->state == TASK_FREE || t->is_idle) {
        return (task_t *)0;
    }
    if (expected_id != 0 && t->id != expected_id) {
        return (task_t *)0;
    }
    task_t *to = t;
    if (t->state == TASK_TERMINATED) {
        if (expected_id == 0 || t->is_thread || t->tgid != t->id) {
            return (task_t *)0;
        }
        to = live_member_for_signal_locked(t->tgid, sig);
        if (!to) {
            return (task_t *)0;
        }
    }
    if (!from_kernel && scheduler_task_is_kernel(to)) {
        return (task_t *)0;
    }
    return to;
}

int scheduler_raise_signal_checked(task_t *t, int expected_id, int sig, int from_kernel) {
    if (!t) {
        return 0;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    task_t *to = signal_target_locked(t, expected_id, sig, from_kernel);
    int woken = 0;
    if (to && sig > 0 && sig <= SIG_MAX) {
        woken = raise_signal_locked(to, sig);
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
    return to != 0;
}

/* M225: SIGCHLD and the two siginfo fields it carries, as one thing under
   the lock. The fields were written before the raise took the lock, on a
   parent looked up without it: two children of one parent ending at once on
   two processors could leave one's pid beside the other's status, and a
   parent slot refilled in between had a stranger's fields written - the
   raise then refused the stranger, but only after. Written now to the task
   the signal actually lands on, after the id is checked. */
static void raise_child_signal(int parent_id, int child_pid, int32_t status) {
    if (parent_id < 0) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    task_t *parent = scheduler_task_by_id(parent_id);
    task_t *to = parent ? signal_target_locked(parent, parent_id, SIGCHLD, 1) : (task_t *)0;
    int woken = 0;
    if (to) {
        to->si_pid = (int32_t)child_pid;
        to->si_status = status;
        woken = raise_signal_locked(to, SIGCHLD);
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    kick_idle_cpus(woken);
}

void scheduler_raise_signal(task_t *t, int sig) {
    (void)scheduler_raise_signal_checked(t, 0, sig, 1);
}

void scheduler_raise_signal_group(int pgid, int sig) {
    if (pgid <= 0) {
        return;
    }
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        int id = t->id;
        if (t->state == TASK_FREE || t->state == TASK_TERMINATED ||
            scheduler_task_is_kernel(t)) {
            continue;
        }
        if (t->pgid == pgid) {
            (void)scheduler_raise_signal_checked(t, id, sig, 0);
        }
    }
}

int scheduler_signal_orderly_stop(task_t *self, int sig) {
    int reached = 0;
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        int id = t->id;
        if (t == self || t->state == TASK_FREE || t->state == TASK_TERMINATED ||
            t->parent_id < 0) {
            continue;
        }
        if (scheduler_raise_signal_checked(t, id, sig, 1)) {
            reached++;
        }
    }
    return reached;
}

int scheduler_task_is_kernel(const task_t *t) {
    return t && (t->kernel_task || t->is_idle);
}

task_t *scheduler_task_for_pid_argument(int pid) {
    return pid == 0 ? scheduler_current() : scheduler_task_by_id(pid);
}

/* setpgid(2), which was sys_setpgid's own body. M225 adds the kernel: a
   kernel thread started inside a program's system call (wifi-dhcp) has that
   program for its parent, so setpgid(its pid, mine) moved a kernel thread
   into a group the program could signal; and a group whose "leader" is a
   kernel task is not one a program can join. */
int scheduler_set_process_group(task_t *self, int pid, int pgid) {
    if (!self) {
        return -1;
    }
    task_t *t = (pid == 0) ? self : scheduler_task_by_id(pid);
    if (!t || t->state == TASK_TERMINATED || t->state == TASK_FREE) {
        return -1;
    }
    if (t != self && t->parent_id != self->tgid) {
        return -1;
    }
    if (scheduler_task_is_kernel(t) && !scheduler_task_is_kernel(self)) {
        return -1;
    }
    if (pgid < 0) {
        return -1;
    }
    if (pgid == 0) {
        pgid = t->id;
    }
    if (pgid != t->id) {
        task_t *leader = scheduler_task_by_id(pgid);
        if (!leader || leader->sid != t->sid ||
            (scheduler_task_is_kernel(leader) && !scheduler_task_is_kernel(t))) {
            return -1;
        }
    }
    t->pgid = pgid;
    return 0;
}

/* M225: the most tasks of process `pid` that have been alive at the same
   time - 1 for a process that never made a thread. -1 once the slot is not
   that process's (released, or never was). [m79] used to SAMPLE this, every
   25 ms, by counting the tasks on threadtest's page table: on a fast machine
   threadtest's three tasks came and went between two samples, and the check
   passed only because a process that has ended stands in the kernel's page
   table, which every kernel thread shares - it counted those. */
int scheduler_thread_group_peak(int pid) {
    int peak = -1;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    int slot = PID_SLOT(pid);
    if (pid > 0 && slot < task_count && tasks[slot].state != TASK_FREE &&
        tasks[slot].generation == PID_GEN(pid) && tasks[slot].tgid == pid) {
        peak = (int)tasks[slot].group_peak;
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return peak;
}

/* M225: SYS_wait(pid) - the kernel's own wait, which pthread_join and the
   boot self-tests use - asks this first. Not the kernel's tasks unless the
   kernel is asking: a process that waited on pid 0 waited on the kernel for
   ever, and one that waited on journal's pid would have reaped it had it
   ever ended. And not the caller itself, which cannot end while it waits. */
int scheduler_may_wait_for(const task_t *self, const task_t *t) {
    if (!self || !t || t == self) {
        return 0;
    }
    return !scheduler_task_is_kernel(t) || scheduler_task_is_kernel(self);
}

/* M225: kill(2), which was sys_kill's own loop. It treated process group 0
   as a real group, and group 0 was the kernel's: the boot task, every idle
   task, journal, tcp-timer and every program the kernel had started. So
   kill(0, SIGKILL) from any of those programs - or from the kernel itself,
   as the battery's M105 stage did by reading a pid of 0 out of a released
   task - reached all of them, and a shell holding CAP_KILL_ANY could end
   the journal with `kill <its pid>`. scheduler_raise_signal_group always
   refused group 0; this is the one rule for every way kill(2) names a
   target, here rather than in the system call so the host tests can ask it:

   - a kernel task is never reached by a caller that is not the kernel
     (the kernel's own self-tests signal the kernel threads they start);
   - group 0 is not a group, so kill(0) from a task in it and kill(-0)
     reach nobody - and since programs the kernel starts lead their own
     group, the only tasks still in it are the kernel's;
   - kill(-1) is every process the caller may signal except its own and
     the kernel's. It used to mean "process group 1", which is a real group
     now that a program in slot 1 leads its own.

   The task table is walked without the lock, as it always was, and every
   check made here is made again where it counts: each target goes to
   scheduler_raise_signal_checked with the id this walk read, which under
   the scheduler lock refuses a slot that has since ended or changed hands
   (an id is never reused - the generation is in it) and a kernel task when
   the sender is not the kernel. */
long scheduler_kill(task_t *self, long pid, int sig,
                    int (*may_signal)(task_t *self, task_t *target)) {
    if (sig < 0 || sig > SIG_MAX) {
        return -OS_ERROR_INVALID;
    }
    if (!self) {
        return -OS_ERROR_SEARCH;
    }
    const int from_kernel = scheduler_task_is_kernel(self);

    if (pid > 0) {
        if (pid > 0x7FFFFFFFL) {
            return -OS_ERROR_SEARCH;
        }
        task_t *t = scheduler_task_by_id((int)pid);
        /* M225: a process whose main thread has gone is still a process
           while any thread of it runs - the signal goes to one of those
           (signal_target_locked). An ended thread, or a process with
           nothing left running, is ESRCH. */
        if (!t || (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_TERMINATED &&
                   scheduler_process_has_ended(t))) {
            return -OS_ERROR_SEARCH;
        }
        if (scheduler_task_is_kernel(t) && !from_kernel) {
            return -OS_ERROR_PERMISSION;
        }
        if (!may_signal(self, t)) {
            return -OS_ERROR_PERMISSION;
        }
        /* Asked again where it cannot change: signal 0 included, because
           "is it there" is the whole of what signal 0 asks. */
        if (!scheduler_raise_signal_checked(t, (int)pid, sig, from_kernel)) {
            return -OS_ERROR_SEARCH;
        }
        return 0;
    }

    const int everyone = (pid == -1);
    int group = 0;
    if (!everyone) {
        if (pid < -0x7FFFFFFFL) {
            return -OS_ERROR_SEARCH;
        }
        group = (pid == 0) ? self->pgid : (int)(-pid);
        if (group <= 0) {
            return -OS_ERROR_SEARCH;
        }
    }

    int delivered = 0;
    int refused = 0;
    int total = task_count;
    for (int i = 0; i < total; i++) {
        task_t *m = &tasks[i];
        /* The id first: whatever the checks below read, the raise refuses a
           slot that is no longer this id (scheduler_raise_signal_checked). */
        int id = m->id;
        if (m->state == TASK_FREE || m->state == TASK_TERMINATED) {
            continue;
        }
        if (everyone) {
            if (scheduler_task_is_kernel(m) || m->tgid == self->tgid) {
                continue;
            }
        } else {
            if (m->pgid != group) {
                continue;
            }
            if (scheduler_task_is_kernel(m) && !from_kernel) {
                refused++;
                continue;
            }
        }
        if (!may_signal(self, m)) {
            refused++;
            continue;
        }
        if (!scheduler_raise_signal_checked(m, id, sig, from_kernel)) {
            continue;
        }
        delivered++;
    }
    if (delivered > 0) {
        return 0;
    }
    return refused ? -OS_ERROR_PERMISSION : -OS_ERROR_SEARCH;
}

int scheduler_signal_pending(void) {
    task_t *t = current_task_now();
    return (t->sig_pending & ~t->sig_blocked) != 0;
}

/* M225 (process-lifetimes): the two records are read by OTHER tasks - procfs
   answers /proc/<pid>/cmdline for anybody, and a thread's spawn, exec and
   fork read its leader's environment - while the owner's reap or exec frees
   or replaces them. So a block is only ever taken off a task, or put on one,
   under scheduler_lock, and freed after; and a reader copies it out under
   the same lock (scheduler_copy_cmdline, scheduler_copy_env) instead of being
   handed a pointer. scheduler_cmdline() used to return the owner's block
   itself, with no lock, and a /proc read that raced the process's end read
   what the reap had just freed. */
void scheduler_release_cmdline(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    char *block = t->cmdline_block;
    t->cmdline_block = NULL;
    t->cmdline_length = 0;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    if (block) {
        kfree(block);
    }
}

void scheduler_set_cmdline(task_t *t, const char *const *argv) {
    if (!t) {
        return;
    }
    uint32_t length = 0;
    char *block = scheduler_pack_cmdline(argv, &length);
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    char *old = t->cmdline_block;
    t->cmdline_block = block;
    t->cmdline_length = block ? length : 0;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    if (old) {
        kfree(old);
    }
}

char *scheduler_pack_cmdline(const char *const *argv, uint32_t *length_out) {
    *length_out = 0;
    if (!argv || !argv[0]) {
        return (char *)0;
    }
    uint32_t needed = 0;
    for (int i = 0; argv[i]; i++) {
        uint32_t length = (uint32_t)k_strlen(argv[i]) + 1;
        if (needed + length > TASK_CMDLINE_MAX) {
            break;
        }
        needed += length;
    }
    if (needed == 0) {
        return (char *)0;
    }
    char *block = (char *)kmalloc(needed);
    if (!block) {
        return (char *)0;
    }
    uint32_t at = 0;
    for (int i = 0; argv[i]; i++) {
        uint32_t length = (uint32_t)k_strlen(argv[i]) + 1;
        if (at + length > needed) {
            break;
        }
        k_memcpy(block + at, argv[i], length);
        at += length;
    }
    *length_out = at;
    return block;
}

/* Whose records a task's are: a thread has none of its own, and
   /proc/<tid>/cmdline is defined to answer with the process's. Under the
   lock, so the leader found is the leader still. */
static task_t *record_owner_locked(task_t *t) {
    if (!t || !t->is_thread) {
        return t;
    }
    task_t *leader = scheduler_task_by_id(t->tgid);
    return leader ? leader : t;
}

int scheduler_copy_cmdline(int pid, char *out, uint32_t capacity) {
    int copied = -1;
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    task_t *t = scheduler_task_by_id(pid);
    if (t && out) {
        const task_t *owner = record_owner_locked(t);
        copied = 0;
        if (owner->cmdline_block) {
            uint32_t length = owner->cmdline_length;
            if (length > capacity) {
                length = capacity;
            }
            k_memcpy(out, owner->cmdline_block, length);
            copied = (int)length;
        } else {
            /* Nothing recorded - a kernel task, or a record that could not
               be allocated: the name, as one argument. */
            uint32_t at = 0;
            for (const char *c = t->name; *c && at + 1 < capacity; c++) {
                out[at++] = *c;
            }
            if (at < capacity) {
                out[at++] = '\0';
            }
            copied = (int)at;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return copied;
}

uint32_t scheduler_copy_env(task_t *t, char *out, uint32_t capacity, uint32_t *count_out) {
    uint32_t length = 0;
    uint32_t count = 0;
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    const task_t *owner = record_owner_locked(t);
    if (owner && out && owner->env_block && owner->env_count &&
        owner->env_length <= capacity) {
        k_memcpy(out, owner->env_block, owner->env_length);
        length = owner->env_length;
        count = owner->env_count;
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    if (count_out) {
        *count_out = count;
    }
    return length;
}

/* How long t's process's command line (env 0) or environment (env 1)
   record is right now, read under the lock. */
static uint32_t record_length(task_t *t, int env) {
    uint32_t length = 0;
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    const task_t *owner = record_owner_locked(t);
    if (owner) {
        if (env) {
            length = (owner->env_block && owner->env_count) ? owner->env_length : 0;
        } else {
            length = owner->cmdline_block ? owner->cmdline_length : 0;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return length;
}

/* A kmalloc'd copy of t's process's command line (env 0) or environment
   (env 1), copied out under the lock - nothing is allocated under
   scheduler_lock here. Null when there is none; *no_memory says when one
   could not be made.

   M225 (fd-use-holds): sized by the record, and allocated only when there
   is one. It took a scratch block at the record's ceiling (4 KiB) first,
   whatever the record was, and then a second block to trim into - so a fork
   of a process with NO environment could fail with ENOMEM for a copy it was
   never going to make. The length is read under the lock, the block
   allocated outside it, and the copy made under it again; an exec on
   another thread can replace the record in between, and a record that grew
   past the block is simply measured again. */
static char *duplicate_record(task_t *t, int env, uint32_t *length_out, uint32_t *count_out,
                              int *no_memory) {
    *length_out = 0;
    if (count_out) {
        *count_out = 0;
    }
    *no_memory = 0;
    for (;;) {
        uint32_t wanted = record_length(t, env);
        if (wanted == 0) {
            return (char *)0;
        }
        char *copy = (char *)kmalloc(wanted);
        if (!copy) {
            *no_memory = 1;
            return (char *)0;
        }
        uint32_t length = 0;
        uint32_t count = 0;
        int grew = 0;
        if (env) {
            length = scheduler_copy_env(t, copy, wanted, &count);
            grew = length == 0 && record_length(t, 1) > wanted;
        } else {
            uint64_t flags = spin_lock_irqsave(&scheduler_lock);
            const task_t *owner = record_owner_locked(t);
            if (owner && owner->cmdline_block) {
                if (owner->cmdline_length <= wanted) {
                    length = owner->cmdline_length;
                    k_memcpy(copy, owner->cmdline_block, length);
                } else {
                    grew = 1;
                }
            }
            spin_unlock_irqrestore(&scheduler_lock, flags);
        }
        if (length) {
            *length_out = length;
            if (count_out) {
                *count_out = count;
            }
            return copy;
        }
        kfree(copy);
        if (!grew) {
            return (char *)0; /* it went while the block was being found */
        }
    }
}

/* M225 (fd-use-holds): what exec inherits when it is given no environment
   of its own - the process's record, copied out under the lock. */
char *scheduler_duplicate_env(task_t *t, uint32_t *length_out, uint32_t *count_out,
                              int *no_memory) {
    return duplicate_record(t, 1, length_out, count_out, no_memory);
}

void scheduler_release_env(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    char *block = t->env_block;
    t->env_block = NULL;
    t->env_length = 0;
    t->env_count = 0;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    if (block) {
        kfree(block);
    }
}

int scheduler_set_env(task_t *t, const char *block, uint32_t length, uint32_t count) {
    if (!block || length == 0 || count == 0) {
        scheduler_release_env(t);
        return 0;
    }
    char *copy = (char *)kmalloc(length);
    if (!copy) {
        return -1;
    }
    for (uint32_t i = 0; i < length; i++) {
        copy[i] = block[i];
    }
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    char *old = t->env_block;
    t->env_block = copy;
    t->env_length = length;
    t->env_count = count;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    if (old) {
        kfree(old);
    }
    return 0;
}

/* A table starts with one reference - the task it was made for. A thread
   adds one by pointing at the same table; a fork does not, it gets its own
   copy, because after a fork the two processes' descriptors move
   independently. */
file_descriptor_table_t *descriptor_table_new(void) {
    file_descriptor_table_t *table =
        (file_descriptor_table_t *)kmalloc(sizeof(file_descriptor_table_t));
    if (!table) {
        return (file_descriptor_table_t *)0;
    }
    k_memset(table, 0, sizeof(*table));
    table->references = 1;
    return table;
}

/* M225: the count is atomic, and it takes no lock, because its two sides
   never shared one: a reference is taken under scheduler_lock (a thread
   being made) and dropped outside it (every exit, and the reap), on as many
   processors as the process has threads. It was `references++` and
   `--references` - a load, an add and a store, with nothing to stop a second
   processor's load landing between the first one's load and store. A lost
   decrement leaks the table and every descriptor in it; a lost increment
   leaves the count one short, so the table is closed and freed while a thread
   still uses it and freed AGAIN when the last one lets go - which is what an
   eight-processor hvf battery panicked on at [m79]'s thread churn:
   "[heap] second kfree of ... from descriptor_table_release".

   A reference is only ever taken by a task that already holds one (the
   creating thread, for the thread it creates), so the count is at least one
   while it is taken and a table at zero is never brought back. That is a
   rule, not luck, so it is checked. */
void descriptor_table_reference(file_descriptor_table_t *table) {
    if (table) {
        /* Relaxed: the caller's own reference already keeps the table alive
           and orders nothing new. */
        int before = __atomic_fetch_add(&table->references, 1, __ATOMIC_RELAXED);
        if (before <= 0) {
            panic("descriptor_table_reference: a reference to a table nobody holds");
        }
    }
}

void descriptor_table_release(file_descriptor_table_t *table) {
    if (!table) {
        return;
    }
    /* Release, so every write this task made to the slots is visible to
       whoever closes them; acquire, so the one that reaches zero sees every
       other task's writes before it closes and frees. */
    int after = __atomic_sub_fetch(&table->references, 1, __ATOMIC_ACQ_REL);
    if (after > 0) {
        return;
    }
    if (after < 0) {
        panic("descriptor_table_release: released more often than it was referenced");
    }
    /* The last task pointing here is gone, so every descriptor in it closes
       now - once, which is the whole reason the count is here. */
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        file_descriptor_release(&table->slots[i]);
    }
    kfree(table);
}

void file_descriptor_release(file_descriptor_slot_t *slot) {
    slot->cloexec = 0;
    slot->nonblock = 0;
    switch (slot->type) {
    case FILE_DESCRIPTOR_PIPE_READ:
        pipe_unref_read(slot->pipe);
        break;
    case FILE_DESCRIPTOR_PIPE_WRITE:
        pipe_unref_write(slot->pipe);
        break;
    case FILE_DESCRIPTOR_FILE:
        open_file_unref(slot->file);
        break;
    case FILE_DESCRIPTOR_SOCKET:
        socket_unref(slot->sock);
        break;
    case FILE_DESCRIPTOR_UNIX:
        unix_socket_unref(slot->un);
        break;
    case FILE_DESCRIPTOR_EVENT:
        eventfd_unref(slot->event);
        break;
    case FILE_DESCRIPTOR_TIMER:
        timerfd_unref(slot->timer);
        break;
    case FILE_DESCRIPTOR_EPOLL:
        epoll_unref(slot->epoll);
        break;
    case FILE_DESCRIPTOR_MEMFD:
        memfd_unref(slot->memfd);
        break;
    default:
        break;
    }
    slot->type = FILE_DESCRIPTOR_NONE;
    slot->pipe = (struct pipe *)0;
}

void file_descriptor_retain(const file_descriptor_slot_t *slot) {
    switch (slot->type) {
    case FILE_DESCRIPTOR_PIPE_READ:
        pipe_reference_read(slot->pipe);
        break;
    case FILE_DESCRIPTOR_PIPE_WRITE:
        pipe_reference_write(slot->pipe);
        break;
    case FILE_DESCRIPTOR_FILE:
        open_file_reference(slot->file);
        break;
    case FILE_DESCRIPTOR_SOCKET:
        socket_reference(slot->sock);
        break;
    case FILE_DESCRIPTOR_UNIX:
        unix_socket_reference(slot->un);
        break;
    case FILE_DESCRIPTOR_EVENT:
        eventfd_reference(slot->event);
        break;
    case FILE_DESCRIPTOR_TIMER:
        timerfd_reference(slot->timer);
        break;
    case FILE_DESCRIPTOR_EPOLL:
        epoll_reference(slot->epoll);
        break;
    case FILE_DESCRIPTOR_MEMFD:
        memfd_reference(slot->memfd);
        break;
    default:
        break;
    }
}

/* M225: the slot protocol.

   The object counts were locked (8084b9d audited every one); what was not
   was the SLOT. sys_close looked at a slot's type, saw something open, and
   called file_descriptor_release on it - and two threads of one process
   closing the same descriptor at once both looked, both saw it open and both
   released it, so one reference was dropped twice. A socket, a unix socket,
   an eventfd, a timerfd, an epoll set or a memfd went to zero with a
   descriptor still counted and was freed twice; a pipe end or an open file
   clamps at zero, which hides the same thing: a reference another holder
   still counts, gone. Every path that COPIED a slot and then took a reference
   - fork's copy, spawn's inherited descriptors, dup, dup2, F_DUPFD,
   /proc/self/fd reopen, SCM_RIGHTS - had the same window the other way
   round: the copy is made, a sibling closes the original and drops the last
   reference, and the copy's reference is taken on an object that is gone.

   So the table has a lock, and these are the only places that take a
   descriptor out of a slot or copy one with a reference of its own. Under
   it: reading and writing slots, and taking a reference (the object's own
   lock, briefly). Never under it: letting an object go, which can close a
   TCP connection or wake a peer - a slot is DETACHED under the lock and the
   caller releases what it detached after. It is never taken under
   scheduler_lock (M204: a pipe writer holds pipe_lock while it takes
   scheduler_lock, so a reference taken under scheduler_lock is the two in
   opposite orders).

   Claiming a free slot is still the M192 compare-and-swap from NONE to
   RESERVED, with no lock: a claimer only ever touches a NONE slot, and the
   paths here treat NONE and RESERVED alike as "not open" and write a NONE
   slot only after claiming it the same way. */
static uint64_t descriptor_table_lock(file_descriptor_table_t *table) {
    return spin_lock_irqsave(&table->lock);
}

static void descriptor_table_unlock(file_descriptor_table_t *table, uint64_t flags) {
    spin_unlock_irqrestore(&table->lock, flags);
}

static file_descriptor_type_t slot_type(const file_descriptor_slot_t *slot) {
    return __atomic_load_n(&slot->type, __ATOMIC_ACQUIRE);
}

static int slot_is_live(const file_descriptor_slot_t *slot) {
    file_descriptor_type_t type = slot_type(slot);
    return type != FILE_DESCRIPTOR_NONE && type != FILE_DESCRIPTOR_RESERVED;
}

/* Writes everything but the type, then the type: a reader that sees the
   type sees the object with it. Called with the lock held and the slot
   claimed (RESERVED) by the caller. */
static void slot_publish(file_descriptor_slot_t *into, const file_descriptor_slot_t *from) {
    into->pipe = from->pipe;
    into->cloexec = from->cloexec;
    into->nonblock = from->nonblock;
    into->writable = from->writable;
    __atomic_store_n(&into->type, from->type, __ATOMIC_RELEASE);
}

/* The mirror image of slot_publish: the type goes FIRST, then the rest.
   slot_publish's rule is that a reader that sees the type sees the object
   with it, and a clear that emptied the object before the type broke it -
   between the two stores a slot said "pipe" and named nothing, and a reader
   that checks its copy by reading the type again (type, object, type) could
   see the same live type twice around a null object. With the type first,
   a reader that finds the object already gone finds the type gone too: the
   release fence orders the type's store before every store after it. The
   claim CAS that can follow the NONE is harmless here - a claimant fills
   its slot only under this lock, which the caller still holds. */
static void slot_clear(file_descriptor_slot_t *slot) {
    __atomic_store_n(&slot->type, FILE_DESCRIPTOR_NONE, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&slot->pipe, (struct pipe *)0, __ATOMIC_RELAXED);
    slot->cloexec = 0;
    slot->nonblock = 0;
    slot->writable = 0;
}

static int slot_claim(file_descriptor_slot_t *slot) {
    file_descriptor_type_t expected = FILE_DESCRIPTOR_NONE;
    return slot_type(slot) == FILE_DESCRIPTOR_NONE &&
           __atomic_compare_exchange_n(&slot->type, &expected, FILE_DESCRIPTOR_RESERVED, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* Numbering a descriptor is claiming it (M192): the lowest free slot at or
   above `from`, taken from NONE to RESERVED so exactly one claimant wins
   it. The claimant fills it with file_descriptor_install or gives it back. */
int file_descriptor_claim(file_descriptor_table_t *table, int from) {
    for (int i = from < 0 ? 0 : from; i < MAX_FILE_DESCRIPTORS; i++) {
        if (slot_claim(&table->slots[i])) {
            return i;
        }
    }
    return -1;
}

void file_descriptor_unclaim(file_descriptor_table_t *table, int fd) {
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return;
    }
    file_descriptor_type_t expected = FILE_DESCRIPTOR_RESERVED;
    __atomic_compare_exchange_n(&table->slots[fd].type, &expected, FILE_DESCRIPTOR_NONE, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* The object's reference passes from the caller to the slot. */
void file_descriptor_install(file_descriptor_table_t *table, int fd,
                             const file_descriptor_slot_t *slot) {
    uint64_t flags = descriptor_table_lock(table);
    if (slot_type(&table->slots[fd]) != FILE_DESCRIPTOR_RESERVED) {
        panic("file_descriptor_install: a slot nobody claimed");
    }
    slot_publish(&table->slots[fd], slot);
    descriptor_table_unlock(table, flags);
}

/* A reference of the caller's own on what `fd` names, so the object outlives
   a sibling closing the descriptor while the caller still uses it - or
   passes it on (SCM_RIGHTS, /proc/self/fd). Given back with
   file_descriptor_release(out). */
int file_descriptor_hold(file_descriptor_table_t *table, int fd, file_descriptor_slot_t *out) {
    out->type = FILE_DESCRIPTOR_NONE;
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return DESCRIPTOR_TABLE_BAD;
    }
    uint64_t flags = descriptor_table_lock(table);
    if (!slot_is_live(&table->slots[fd])) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_BAD;
    }
    *out = table->slots[fd];
    file_descriptor_retain(out);
    descriptor_table_unlock(table, flags);
    return 0;
}

/* M225 (fd-use-holds): USING a descriptor - read, write, send, recv, ioctl,
   fcntl, epoll, poll's readiness, the socket calls - is Linux's fdget/fdput.
   Every one of those read the slot and used its object with no reference of
   its own, so a sibling thread's close() in the middle of the call dropped
   the object's last reference under it: a unix socket, socket, eventfd,
   timerfd, epoll set or memfd was freed while the call still used it (a
   blocked read woke into freed memory, or re-read the slot after the wait
   and found nothing there), and a pipe end or an open file - whose counts
   clamp at zero - lost a reference somebody else still counted.

   Two ways, as Linux has (fdget's "light" path):
   - The table is this task's alone (one reference: no other thread names
     it). Nothing can close or replace a slot but this task, which is in this
     call, so the slot is copied and nothing is counted. A table only gains a
     name from a task that already names it (task_spawn_common), so it stays
     this task's alone for the length of the call. This is every
     single-threaded program, and it costs a load and a copy - inline, in
     scheduler.h (file_descriptor_get), because a call and a frame were most
     of what it cost under TCG.
   - Shared: here. The copy and a reference of the call's own, taken under
     the table's lock (as file_descriptor_hold does), and the use put on the
     task's list so an exit in the middle of the call gives it back - except
     for what needs no reference (use_takes_reference: the console, and a
     named pipe, which is never freed and whose let-go wakes its watchers).

   What a call sees when a sibling closes its descriptor part way through:
   the call goes on with the object it started with, which stays alive until
   the call lets go - Linux's answer for every case this machine has. A read
   blocked on a pipe, a socket, an eventfd or a timerfd keeps waiting on that
   object and returns what arrives on it (close() does not wake it, on Linux
   either; shutdown() is how a program ends another thread's wait); the next
   call that names the number is told it is not open. */
/* Whether a use of what `slot` names needs a reference of its own. The
   console's two kinds name no object. A named pipe (pipe_named, which every
   window's event pipe and the compositor's request pipes are) is made once
   and never freed or reused, so it cannot go away under a call - and it is
   never counted down either: pipe_unref_* on one only wakes everybody
   watching it, which is what a close of it is meant to do. Counting a USE of
   one made every poll and epoll_wait in a threaded process wake ITSELF -
   the scan's put matched the poller's own watch, set watch_fired, and
   scheduler_watch_block returned at once - so an idle threaded client
   watching its window's events (Chromium's UI thread) spun a core until its
   timeout, or for ever; and each use added one to the pipe's reader or
   writer count for good. */
static int use_takes_reference(const file_descriptor_slot_t *slot) {
    switch (slot->type) {
    case FILE_DESCRIPTOR_STDIN:
    case FILE_DESCRIPTOR_STDOUT:
        return 0;
    case FILE_DESCRIPTOR_PIPE_READ:
    case FILE_DESCRIPTOR_PIPE_WRITE:
        return !pipe_is_persistent(slot->pipe);
    default:
        return 1;
    }
}

int file_descriptor_get_shared(task_t *t, int fd, file_descriptor_use_t *use) {
    use->counted = 0;
    use->slot.type = FILE_DESCRIPTOR_NONE;
    file_descriptor_table_t *table = t ? t->descriptor_table : (file_descriptor_table_t *)0;
    if (!table || fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return DESCRIPTOR_TABLE_BAD;
    }
    /* file_descriptor_hold, but a use that needs no reference takes none. */
    uint64_t flags = descriptor_table_lock(table);
    if (!slot_is_live(&table->slots[fd])) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_BAD;
    }
    use->slot = table->slots[fd];
    int counted = use_takes_reference(&use->slot);
    if (counted) {
        file_descriptor_retain(&use->slot);
    }
    descriptor_table_unlock(table, flags);
    if (counted) {
        use->counted = 1;
        use->next = t->descriptor_uses;
        t->descriptor_uses = use;
    }
    return 0;
}

/* What kind of thing `fd` names right now, and nothing more: no object is
   touched, so nothing has to be held. For a poller deciding which address
   to be woken by (the console's input or the object's own), where a stale
   answer costs one spurious or one missed-until-the-next-scan wake. */
file_descriptor_type_t file_descriptor_peek_type(task_t *t, int fd) {
    file_descriptor_table_t *table = t ? t->descriptor_table : (file_descriptor_table_t *)0;
    if (!table || fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return FILE_DESCRIPTOR_NONE;
    }
    return slot_type(&table->slots[fd]);
}

/* Taken off the list before it is let go of: an exit that strikes between
   the two leaks the one reference (as a close() struck between detaching
   and releasing does) rather than giving it back twice. */
void file_descriptor_put(task_t *t, file_descriptor_use_t *use) {
    if (!use->counted) {
        return;
    }
    for (file_descriptor_use_t **at = &t->descriptor_uses; *at; at = &(*at)->next) {
        if (*at == use) {
            *at = use->next;
            break;
        }
    }
    use->counted = 0;
    use->next = (file_descriptor_use_t *)0;
    file_descriptor_release(&use->slot);
}

/* M225 (fd-use-holds): the rule above, checked where it can be - as a
   system call returns to its program. A use left on the list there is a
   record in a stack frame that has returned; the next get links in front of
   it and an exit's put_all would release whatever that stack memory holds
   by then. Said at once instead, while the task's kernel_activity still
   names the call that left it. */
void file_descriptor_uses_settled(task_t *t) {
    if (t && t->descriptor_uses) {
        panic("file_descriptor_uses_settled: a system call returned without putting a descriptor it got");
    }
}

/* The exit's: every use the task's unfinished call still holds. Run by the
   task itself, on its own stack, which is where the records are. */
void file_descriptor_put_all(task_t *t) {
    while (t->descriptor_uses) {
        file_descriptor_use_t *use = t->descriptor_uses;
        t->descriptor_uses = use->next;
        use->counted = 0;
        use->next = (file_descriptor_use_t *)0;
        file_descriptor_release(&use->slot);
    }
}

/* close(): the slot is emptied under the lock and what it held handed to the
   caller to release - exactly one of any number of threads closing one
   descriptor at once gets it, and the rest are told it was not open. */
int file_descriptor_detach(file_descriptor_table_t *table, int fd, file_descriptor_slot_t *out) {
    out->type = FILE_DESCRIPTOR_NONE;
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return DESCRIPTOR_TABLE_BAD;
    }
    uint64_t flags = descriptor_table_lock(table);
    if (!slot_is_live(&table->slots[fd])) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_BAD;
    }
    *out = table->slots[fd];
    slot_clear(&table->slots[fd]);
    descriptor_table_unlock(table, flags);
    return 0;
}

/* dup2(): `newfd` becomes a second reference to what `oldfd` names, and what
   `newfd` held before is handed back in `displaced` for the caller to
   release after. A slot another thread is still filling in is not replaced
   (Linux says EBUSY for the same window): its owner is about to install into
   it, and one of the two objects would be lost. */
int descriptor_table_duplicate(file_descriptor_table_t *table, int oldfd, int newfd,
                               int cloexec, file_descriptor_slot_t *displaced) {
    displaced->type = FILE_DESCRIPTOR_NONE;
    if (oldfd < 0 || oldfd >= MAX_FILE_DESCRIPTORS || newfd < 0 ||
        newfd >= MAX_FILE_DESCRIPTORS) {
        return DESCRIPTOR_TABLE_BAD;
    }
    uint64_t flags = descriptor_table_lock(table);
    if (!slot_is_live(&table->slots[oldfd])) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_BAD;
    }
    if (newfd == oldfd) {
        descriptor_table_unlock(table, flags);
        return newfd;
    }
    file_descriptor_slot_t *into = &table->slots[newfd];
    if (slot_is_live(into)) {
        *displaced = *into;
        /* A live slot changes only under this lock, so it is ours to
           rewrite; RESERVED keeps a lock-free claimer off it meanwhile. */
        __atomic_store_n(&into->type, FILE_DESCRIPTOR_RESERVED, __ATOMIC_RELEASE);
    } else if (!slot_claim(into)) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_BUSY;
    }
    file_descriptor_slot_t copy = table->slots[oldfd];
    copy.cloexec = cloexec ? 1 : 0;
    file_descriptor_retain(&copy);
    slot_publish(into, &copy);
    descriptor_table_unlock(table, flags);
    return newfd;
}

/* dup() and F_DUPFD: the lowest free slot at or above `from`. */
int descriptor_table_duplicate_lowest(file_descriptor_table_t *table, int oldfd, int from,
                                      int cloexec) {
    if (oldfd < 0 || oldfd >= MAX_FILE_DESCRIPTORS) {
        return DESCRIPTOR_TABLE_BAD;
    }
    uint64_t flags = descriptor_table_lock(table);
    if (!slot_is_live(&table->slots[oldfd])) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_BAD;
    }
    int fd = file_descriptor_claim(table, from);
    if (fd < 0) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_FULL;
    }
    file_descriptor_slot_t copy = table->slots[oldfd];
    copy.cloexec = cloexec ? 1 : 0;
    file_descriptor_retain(&copy);
    slot_publish(&table->slots[fd], &copy);
    descriptor_table_unlock(table, flags);
    return fd;
}

/* fork() and spawn: every open descriptor of `from` copied into `to` with a
   reference of its own, under `from`'s lock so none of them can be closed
   between the copy and the reference. `to` is nobody else's yet. A spawn is
   an exec (`for_exec`), so close-on-exec descriptors stay behind, as does
   everything from `keep_below` up. */
void descriptor_table_copy(file_descriptor_table_t *to, file_descriptor_table_t *from,
                           int for_exec, int keep_below) {
    if (!from) {
        return; /* `to` came from descriptor_table_new: empty already */
    }
    uint64_t flags = descriptor_table_lock(from);
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        const file_descriptor_slot_t *source = &from->slots[i];
        if (!slot_is_live(source) || i >= keep_below || (for_exec && source->cloexec)) {
            to->slots[i].type = FILE_DESCRIPTOR_NONE;
            to->slots[i].pipe = (struct pipe *)0;
            to->slots[i].cloexec = 0;
            to->slots[i].nonblock = 0;
            to->slots[i].writable = 0;
            continue;
        }
        to->slots[i] = *source;
        file_descriptor_retain(&to->slots[i]);
    }
    descriptor_table_unlock(from, flags);
}

/* fcntl F_SETFD / F_SETFL: the descriptor's own flags, written under the
   lock so a dup or a fork copying the slot at the same moment copies it
   whole, and so a slot that has been closed meanwhile is not written to.
   -1 leaves a flag as it is. */
int file_descriptor_set_flags(file_descriptor_table_t *table, int fd, int cloexec,
                              int nonblock) {
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return DESCRIPTOR_TABLE_BAD;
    }
    uint64_t flags = descriptor_table_lock(table);
    if (!slot_is_live(&table->slots[fd])) {
        descriptor_table_unlock(table, flags);
        return DESCRIPTOR_TABLE_BAD;
    }
    if (cloexec >= 0) {
        table->slots[fd].cloexec = cloexec ? 1 : 0;
    }
    if (nonblock >= 0) {
        table->slots[fd].nonblock = nonblock ? 1 : 0;
    }
    descriptor_table_unlock(table, flags);
    return 0;
}

/* exec(): the next close-on-exec descriptor at or above `from`, detached for
   the caller to release. One at a time, so the lock is never held across a
   release and never for more than one scan. */
int descriptor_table_detach_cloexec(file_descriptor_table_t *table, int from,
                                    file_descriptor_slot_t *out) {
    out->type = FILE_DESCRIPTOR_NONE;
    uint64_t flags = descriptor_table_lock(table);
    for (int i = from < 0 ? 0 : from; i < MAX_FILE_DESCRIPTORS; i++) {
        if (slot_is_live(&table->slots[i]) && table->slots[i].cloexec) {
            *out = table->slots[i];
            slot_clear(&table->slots[i]);
            descriptor_table_unlock(table, flags);
            return i;
        }
    }
    descriptor_table_unlock(table, flags);
    return DESCRIPTOR_TABLE_BAD;
}

/* M225: a task's table pointer is taken away under scheduler_lock, and the
   reference it stood for is dropped after. Other code reads ANOTHER task's
   table - /proc's task list counting open descriptors, the high-water mark -
   and does it under scheduler_lock with the pointer checked: while the lock
   is held a non-null pointer is a reference its task has not dropped yet, so
   the table cannot be freed under the reader. Dropping first and clearing
   after left a window in which the pointer named a freed table. */
static file_descriptor_table_t *take_descriptor_table(task_t *t) {
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    file_descriptor_table_t *table = t->descriptor_table;
    t->descriptor_table = (file_descriptor_table_t *)0;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return table;
}

int scheduler_open_descriptor_count(task_t *t) {
    if (!t) {
        return 0;
    }
    int open = 0;
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    const file_descriptor_table_t *table = t->descriptor_table;
    for (int i = 0; table && i < MAX_FILE_DESCRIPTORS; i++) {
        if (table->slots[i].type != FILE_DESCRIPTOR_NONE) {
            open++;
        }
    }
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return open;
}

/* M225 (process-lifetimes): record locks belong to the PROCESS. POSIX's
   fcntl(F_SETLK) locks are owned by the process - any of its threads may
   unlock one, a second thread is not in conflict with its own process's
   lock, F_GETLK names the process - so they are keyed by what getpid()
   answers, the thread-group id, and not by the task that asked. They were
   keyed by the task: a thread's lock conflicted with its own process, was
   reported with a tid, and went when that THREAD exited. */
int scheduler_record_lock_owner(const task_t *t) {
    return t ? t->tgid : 0;
}

/* Whether any task still names this table. A task's pointer is cleared under
   scheduler_lock before its reference is dropped (take_descriptor_table),
   and a new name is only ever added by a task that already has one
   (may_add_a_thread_to_locked) - so once this is false it stays false. */
static int descriptor_table_named_locked(const file_descriptor_table_t *table) {
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_FREE && tasks[i].descriptor_table == table) {
            return 1;
        }
    }
    return 0;
}

void scheduler_release_file_descriptors(task_t *t) {
    /* One reference, not one pass over the slots: the descriptors close when
       the LAST task sharing this table is gone, and a thread exiting while
       its siblings run is not that. */
    file_descriptor_table_t *table = take_descriptor_table(t);
    if (!table) {
        return;
    }
    /* And the process's record locks go when the process does, which is the
       same moment: the table is the process's and only its tasks name it.
       Every task looks AFTER clearing its own name, so of two leaving at
       once at least the second sees nobody - the locks cannot be left
       behind - and a release that both make is a release of locks nobody
       can take any more, since every task of the process is on its way out
       (a fork has its own table and its own pid). Released before this
       task is TERMINATED, so a parent whose wait() says the process has
       ended finds its locks gone. */
    uint64_t flags = spin_lock_irqsave(&scheduler_lock);
    int last = !descriptor_table_named_locked(table);
    spin_unlock_irqrestore(&scheduler_lock, flags);
    descriptor_table_release(table);
    if (last && flock_release_pid(scheduler_record_lock_owner(t)) > 0) {
        scheduler_wake_all(FLOCK_CHAN);
    }
}

void scheduler_reset_file_descriptors_to_std(task_t *t) {
    if (!t->descriptor_table) {
        return;
    }
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        file_descriptor_release(&t->descriptor_table->slots[i]);
    }
    t->descriptor_table->slots[0].type = FILE_DESCRIPTOR_STDIN;
    t->descriptor_table->slots[1].type = FILE_DESCRIPTOR_STDOUT;
    t->descriptor_table->slots[2].type = FILE_DESCRIPTOR_STDOUT;
}
