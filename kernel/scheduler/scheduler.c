#include "scheduler.h"

#include "device/pty.h"
#include "device/tty.h"

#include <stddef.h>
#include <stdint.h>

#include "architecture/x86_64/cpu.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/global_descriptor_table.h"
#include "architecture/x86_64/io.h"
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

static void scheduler_deliver_pending_signal(void);

#define TASK_STACK_SIZE (32 * 1024)
#define SCHEDULER_QUANTUM_TICKS 2

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);

static task_t *tasks;
static int task_count;

static task_t *current_task[MAX_CPUS];
static uint32_t ticks_in_slice[MAX_CPUS];
static uint32_t aging_ticks;

static volatile int need_resched[MAX_CPUS];

static int peak_live_tasks;
static uint64_t loaded_pml4_phys[MAX_CPUS];

static spinlock_t scheduler_lock;

static uint64_t event_sequence;

static int blocked_count;

static task_t *pick_next(task_t *from, int cpu);
void scheduler_dump_cpus(void);
static void wake_expired(uint64_t now_ms);
static void fire_expired_alarms(uint64_t now_ms);
static void unblock_self(task_t *self);

const int scheduler_poll_channel = 0;
const int scheduler_keyboard_channel = 0;

static uint64_t idle_ticks[MAX_CPUS];
static uint64_t total_ticks[MAX_CPUS];

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
        cpu_halt();
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

static void set_task_name(task_t *t, const char *name) {
    int i = 0;
    if (name) {
        for (; name[i] && i < TASK_NAME_MAX - 1; i++) {
            t->name[i] = name[i];
        }
    }
    t->name[i] = '\0';
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
    spin_unlock(&scheduler_lock);
    cpu_enable_interrupts();
    task_t *t = current_task[smp_current_cpu()];
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

void scheduler_resume_stopped(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    if (t->state == TASK_STOPPED) {
        t->state = TASK_READY;
        t->prio = PRIO_INTERACTIVE;
        t->full_slices = 0;
        t->wait_chan = (const void *)0;
        t->wake_deadline_ms = 0;
        t->stopped_sig = 0;
        t->stop_reported = 0;
        event_sequence++;
    }
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
        }
    }

    wake_expired(pit_get_ticks() * (1000 / PIT_HZ));
    fire_expired_alarms(pit_get_ticks() * (1000 / PIT_HZ));

    if (t->pending_signal != 0) {
        deliver_pending_signal_and_exit(t);
    }
    if (t->pending_stop != 0) {
        take_pending_stop(t);
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
    tasks[0].parent_id = -1;
    tasks[0].pgid = 0;
    tasks[0].sid = 0;
    tasks[0].caps = CAP_ALL;
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
    t->parent_id = -1;
    t->pgid = 0;
    t->sid = 0;
    t->pending_signal = 0;
    t->reaped = 0;
    t->exit_signal = 0;
    t->caps = CAP_ALL;
    t->is_idle = 1;
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

/* Reset the slots of leaders whose groups have finished since they were
   last looked at.

   scheduler_reap_slot holds a leader's slot while its threads are still
   running, and releases it when the last of them is reaped. A thread nobody
   joins is reaped by nobody, so without this the leader's slot would be held
   until the machine stopped - and a task table with 128 entries in it fills
   up quietly, as spawns that fail rather than as anything that says why.

   Here because it has to run somewhere that can take a lock and wait, and a
   spawn is the moment the answer matters. task_exit cannot do it: a fatal
   signal is delivered on the timer interrupt, and scheduler_reap_slot waits
   for a task to leave its kernel stack. */
static void release_finished_leaders(void) {
    for (int i = 0; i < task_count; i++) {
        task_t *o = &tasks[i];
        if (o->state != TASK_TERMINATED || o->is_thread) {
            continue;
        }
        if (thread_group_has_live_members(o->tgid, o)) {
            continue;
        }
        scheduler_reap_slot(o);
    }
}

static task_t *task_spawn_common(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                                  uint64_t heap_start, uint64_t shared_memory_base, task_t *thread_of) {
    _Static_assert(TASK_STACK_SIZE % 4096 == 0, "a kernel stack must be a whole number of frames");
    release_finished_leaders();
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
            return NULL;
        }
        slot = task_count++;
    }
    task_t *t = &tasks[slot];
    task_t *caller = current_task[smp_current_cpu()];
    t->id = PID_MAKE(slot, t->generation);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->pml4_phys = pml4_phys;
    t->stack_base = stack_base;
    t->kernel_stack_top = (uint64_t)(stack_base + TASK_STACK_SIZE);
    t->home_cpu = -1;

    if (thread_of) {
        /* A thread. It does not get a copy of anything: it points at the
           process's one table, so a descriptor either thread opens is open
           for both and one either closes is closed for both. */
        t->descriptor_table = thread_of->descriptor_table;
        descriptor_table_reference(t->descriptor_table);
    } else {
        t->descriptor_table = fresh;
        for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
            t->descriptor_table->slots[i] = caller->descriptor_table->slots[i];
            /* Close-on-exec, and this IS the exec: a spawn replaces the
               image, which is what the flag is about. A thread above takes
               the other branch and keeps them. */
            if (t->descriptor_table->slots[i].cloexec) {
                t->descriptor_table->slots[i].type = FILE_DESCRIPTOR_NONE;
                t->descriptor_table->slots[i].cloexec = 0;
                continue;
            }
            file_descriptor_retain(&t->descriptor_table->slots[i]);
        }
    }
    t->parent_id = caller->id;
    t->pgid = caller->pgid;
    t->sid = caller->sid;
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
    t->exiting = 0;
    t->caps = caller->caps;
    t->prio = PRIO_INTERACTIVE;
    t->full_slices = 0;
    t->last_block_tick = pit_get_ticks();

    fpu_state_init(t->fpu_state);

    t->heap_brk = heap_start;
    t->heap_mapped_end = heap_start;
    t->shared_memory_next_vaddr = shared_memory_base;
    set_task_name(t, name);

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
        for (int i = 0; i < task_count; i++) {
            if (tasks[i].state != TASK_FREE) {
                live++;
            }
        }
        if (live > peak_live_tasks) {
            peak_live_tasks = live;
        }
    }

    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return t;
}


task_t *task_spawn(const char *name, void (*entry)(void *arg), void *arg) {
    return task_spawn_common(name, virtual_memory_kernel_pml4_phys(), entry, arg, 0, 0, (task_t *)0);
}

task_t *task_spawn_in(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                       uint64_t heap_start, uint64_t shared_memory_base) {
    return task_spawn_common(name, pml4_phys, entry, arg, heap_start, shared_memory_base, (task_t *)0);
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
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
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
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    for (int i = 0; i < n; i++) {
        task_t *t = scheduler_task_by_id(to_signal[i]);
        if (t) {
            scheduler_raise_signal(t, SIGALRM);
        }
    }
}

unsigned int scheduler_set_alarm(task_t *t, unsigned int seconds) {
    if (!t) {
        return 0;
    }
    uint64_t now_ms = pit_get_ticks() * (1000 / PIT_HZ);
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

static void wake_expired(uint64_t now_ms) {
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    if (blocked_count == 0) {
        spin_unlock(&scheduler_lock);
        irq_restore(flags);
        return;
    }
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wake_deadline_ms != 0 &&
            now_ms >= tasks[i].wake_deadline_ms) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].last_block_tick = pit_get_ticks();
            need_resched[smp_current_cpu()] = 1;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
}

uint64_t scheduler_event_sequence(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    uint64_t v = event_sequence;
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return v;
}

int scheduler_wake_n(const void *chan, int max) {
    if (!chan || max <= 0) {
        return 0;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    event_sequence++;
    int woken = 0;
    for (int i = 0; i < task_count && woken < max; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan) {
            blocked_count--;
            tasks[i].state = TASK_READY;
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
    return woken;
}

void scheduler_wake_all(const void *chan) {
    if (!chan) {
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
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan) {
            blocked_count--;
            tasks[i].state = TASK_READY;
            tasks[i].prio = PRIO_INTERACTIVE;
            tasks[i].full_slices = 0;
            tasks[i].wait_chan = (const void *)0;
            tasks[i].wake_deadline_ms = 0;
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
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
    scheduler_take_pending_stop_if_any(current_task[smp_current_cpu()]);

    int cpu = smp_current_cpu();
    uint64_t sflags = irq_save_disable();
    spin_lock(&scheduler_lock);
    if (event_sequence != expected_sequence) {
        spin_unlock(&scheduler_lock);
        irq_restore(sflags);
        return;
    }
    task_t *self = current_task[cpu];
    self->wait_chan = chan;
    self->wake_deadline_ms = deadline_ms;
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
    int cpu = smp_current_cpu();
    task_t *self = current_task[cpu];
    if (!self || self->is_idle) {
        return;
    }
    uint64_t deadline = pit_get_ticks() * (1000 / PIT_HZ) +
                        (uint64_t)(ms ? ms : 1);
    uint64_t sflags = irq_save_disable();
    spin_lock(&scheduler_lock);
    self->wait_chan = (const void *)&sleep_channel;
    self->wake_deadline_ms = deadline;
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&scheduler_lock);
    irq_restore(sflags);

    scheduler_deliver_pending_signal();
    schedule();
    unblock_self(self);
}

void scheduler_block_on(const void *chan, uint64_t deadline_ms, spinlock_t *lock, uint64_t *flags) {
    int cpu = smp_current_cpu();
    uint64_t sflags = irq_save_disable();
    spin_lock(&scheduler_lock);
    task_t *self = current_task[cpu];
    self->wait_chan = chan;
    self->wake_deadline_ms = deadline_ms;
    self->state = TASK_BLOCKED;
    blocked_count++;
    spin_unlock(&scheduler_lock);
    irq_restore(sflags);

    spin_unlock_irqrestore(lock, *flags);

    scheduler_deliver_pending_signal();

    schedule();
    unblock_self(self);
    *flags = spin_lock_irqsave(lock);
}

void schedule(void) {
    int cpu = smp_current_cpu();
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    task_t *previous = current_task[cpu];
    task_t *next = pick_next(previous, cpu);

    if (next == previous) {
        spin_unlock(&scheduler_lock);
        irq_restore(flags);
        return;
    }

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
    }
    next->state = TASK_RUNNING;
    current_task[cpu] = next;

    tss_set_rsp0(cpu, next->kernel_stack_top);
    if (next->pml4_phys != loaded_pml4_phys[cpu]) {
        virtual_memory_switch_address_space(next->pml4_phys);
        loaded_pml4_phys[cpu] = next->pml4_phys;
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

    context_switch(&previous->rsp, next->rsp);

    spin_unlock(&scheduler_lock);
    irq_restore(flags);
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
    task_t *t = current_task[smp_current_cpu()];
    if (t->pending_signal != 0) {
        deliver_pending_signal_and_exit(t);
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
    shared_memory_free_by_owner(t->id);
    scheduler_release_file_descriptors(t);
    scheduler_release_env(t);

    if (t->sid != 0 && t->sid == t->id) {
        tty_release_session(tty_console(), t->sid);
        pty_release_session(t->sid);
    }

    if (t->pml4_phys != virtual_memory_kernel_pml4_phys()) {
        int cpu = smp_current_cpu();
        uint64_t dead = t->pml4_phys;
        uint64_t peak = virtual_memory_rss_peak_pages(dead);
        if (peak > t->max_rss_pages) {
            t->max_rss_pages = peak;
        }
        int others = 0;
        uint64_t eflags = irq_save_disable();
        spin_lock(&scheduler_lock);
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
            t->pml4_phys = virtual_memory_kernel_pml4_phys();
            virtual_memory_switch_address_space(t->pml4_phys);
            loaded_pml4_phys[cpu] = t->pml4_phys;
            process_destroy_address_space(dead);
        }
    }

    t->exit_code = code;
    if (t->state == TASK_BLOCKED) {
        blocked_count--;
    }
    t->state = TASK_TERMINATED;
    scheduler_wake_all((const void *)t);
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    if (t->parent_id >= 0) {
        task_t *parent = scheduler_task_by_id(t->parent_id);
        if (parent && parent != t) {
            parent->si_pid = (int32_t)t->id;
            parent->si_status = t->exit_signal ? (int32_t)(t->exit_signal & 0x7F)
                                               : (int32_t)(t->exit_code & 0xFF);
            scheduler_raise_signal(parent, SIGCHLD);
        }
    }
    schedule();
    panic("task_exit: terminated task resumed");
}

void task_exit(void) {
    task_exit_with_code(0);
}

task_t *scheduler_current(void) {
    return current_task[smp_current_cpu()];
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
static int thread_group_has_live_members(int group, const task_t *except) {
    int live = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    for (int i = 0; i < task_count; i++) {
        const task_t *o = &tasks[i];
        if (o == except || o->state == TASK_FREE || o->state == TASK_TERMINATED) {
            continue;
        }
        if (o->tgid == group) {
            live = 1;
            break;
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return live;
}

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
    const int reaped_group = t->tgid;
    const int reaped_a_thread = t->is_thread;

    /* The command line belongs to the address space, so it goes when the last
       member of the group does - not at task_exit, where a leader that
       outlives its own main thread would take it away from threads that are
       still running and still have a /proc entry. */
    scheduler_release_cmdline(t);

    /* task_exit_with_code released this on the way out and it is normally
       null by now. A task that reached TERMINATED without going through
       that path still holds one, and since M146 the table is a heap
       allocation rather than an array inside the task - so leaving it here
       is a leak rather than nothing. Releasing is idempotent; this is
       before the lock because closing a descriptor can reach a pipe or a
       socket, which is not work for a locked region. */
    if (t->descriptor_table) {
        descriptor_table_release(t->descriptor_table);
        t->descriptor_table = (file_descriptor_table_t *)0;
    }
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

    uint8_t *stack = t->stack_base;
    t->stack_base = NULL;
    t->kernel_stack_top = 0;
    t->home_cpu = -1;
    t->generation++;
    t->state = TASK_FREE;
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
    t->exiting = 0;
    t->tgid = 0;
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
            !leader->is_thread) {
            scheduler_reap_slot(leader);
        }
    }
}

void scheduler_wake_task(task_t *t) {
    if (!t) {
        return;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    event_sequence++;
    if (t->state == TASK_BLOCKED) {
        blocked_count--;
        t->state = TASK_READY;
        t->prio = PRIO_INTERACTIVE;
        t->full_slices = 0;
        t->wait_chan = (const void *)0;
        t->wake_deadline_ms = 0;
    }
    spin_unlock(&scheduler_lock);
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
   and is what a seqlock would close without a reader ever blocking. */
static int mmap_region_snapshot(task_t *self, uint64_t page, mmap_region_t *out) {
    const mmap_region_t *found = mmap_region_for(self, page);
    if (found) {
        *out = *found;
    }
    return found != 0;
}

uint64_t scheduler_regions_lock(task_t *t) {
    return spin_lock_irqsave(&t->mmap_lock);
}

void scheduler_regions_unlock(task_t *t, uint64_t flags) {
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
            for (uint64_t p = from; p < to; p += PAGE_SIZE) {
                if (!virtual_memory_user_range_ok(t->pml4_phys, p, 1, 0)) {
                    continue;
                }
                virtual_memory_unmap_page_in(t->pml4_phys, p);
                dropped++;
            }
            continue;
        }
        if (!t->mmaps[i].shared || t->mmaps[i].handle < 0) {
            continue;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        uint64_t from = start > rstart ? start : rstart;
        uint64_t to = end < rend ? end : rend;
        for (uint64_t p = from; p < to; p += PAGE_SIZE) {
            if (!virtual_memory_user_range_ok(t->pml4_phys, p, 1, 0)) {
                continue;
            }
            virtual_memory_unmap_page_in(t->pml4_phys, p);
            file_mapping_put(t->mmaps[i].handle,
                        t->mmaps[i].file_page +
                            (uint32_t)((p - rstart) / PAGE_SIZE));
            dropped++;
        }
    }
    return dropped;
}

static int fill_one_page_ex(task_t *self, uint64_t page, int for_write, int for_exec) {

    if (virtual_memory_user_range_ok(self->pml4_phys, page, 1, 0)) {
        return 0;
    }

    /* M180: one snapshot, used by the policy and by everything below it. */
    mmap_region_t snapshot;
    const mmap_region_t *region = mmap_region_snapshot(self, page, &snapshot)
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
        return virtual_memory_try_map_page_in(self->pml4_phys, page, phys, flags) == 0
                   ? 1
                   : FILL_NO_MEMORY;
    }
    if (region && region->handle >= 0 && region->shared) {
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        uint64_t sphys = file_mapping_get(region->handle, index,
                                     (region->prot & PROT_WRITE) != 0);
        if (sphys == 0) {
            return FILL_NO_MEMORY;
        }
        if (virtual_memory_try_map_page_in(self->pml4_phys, page, sphys, flags) != 0) {
            file_mapping_put(region->handle, index);
            return FILL_NO_MEMORY;
        }
        return 1;
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
    if (virtual_memory_try_map_page_in(self->pml4_phys, page, phys, flags) != 0) {
        physical_memory_free_frame(phys);
        return FILL_NO_MEMORY;
    }
    return 1;
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
            if (broke == 2 && scheduler_address_space_is_shared(self)) {
                /* The frame moved and a sibling on another core may still be
                   reading the old one. The shootdown waits for every core to
                   answer, and a core answers with an interrupt, so this waits
                   the way sys_fork's does: with interrupts on. A fault from
                   ring 3 is on the task's own kernel stack, like a syscall. */
                cpu_enable_interrupts();
                smp_tlb_shootdown();
                cpu_disable_interrupts();
            }
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
    if (!in_arena && !in_stack) {
        return;
    }
    for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
        if (virtual_memory_user_range_ok(self->pml4_phys, page, 1, for_write)) {
            continue;
        }
        if (for_write) {
            int broke = virtual_memory_cow_break(self->pml4_phys, page);
            if (broke == 2 && scheduler_address_space_is_shared(self)) {
                smp_tlb_shootdown();
            }
            if (broke) {
                continue;
            }
        }
        fill_one_page(self, page, for_write);
    }
}

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

    t->descriptor_table = fresh;
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        t->descriptor_table->slots[i] = parent->descriptor_table->slots[i];
        file_descriptor_retain(&t->descriptor_table->slots[i]);
    }
    t->parent_id = parent->id;
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
    t->env_block = NULL;
    t->cmdline_block = NULL;
    t->cmdline_length = 0;
    t->env_length = 0;
    t->env_count = 0;
    /* A fork has the command line of what it forked from until it execs, and
       a child caught between the two is a real thing to see in a process
       list: Chromium's launcher forks and then execs /proc/self/exe. */
    if (space->cmdline_block && space->cmdline_length) {
        char *copy = (char *)kmalloc(space->cmdline_length);
        if (copy) {
            k_memcpy(copy, space->cmdline_block, space->cmdline_length);
            t->cmdline_block = copy;
            t->cmdline_length = space->cmdline_length;
        }
    }

    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = parent->sig_handler[i];
    }
    t->sig_restorer = parent->sig_restorer;
    t->sig_blocked = parent->sig_blocked;
    t->sig_pending = 0;
    t->sig_siginfo = parent->sig_siginfo;
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
    t->exiting = 0;
    t->caps = parent->caps;

    for (size_t i = 0; i < sizeof(t->fpu_state); i++) {
        t->fpu_state[i] = parent->fpu_state[i];
    }

    t->fs_base = parent->fs_base;

    t->heap_brk = space->heap_brk;
    t->heap_mapped_end = space->heap_mapped_end;
    t->shared_memory_next_vaddr = space->shared_memory_next_vaddr;
    set_task_name(t, parent->name);

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

    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    return t;
}

task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg) {
    return task_spawn_common(name, leader->pml4_phys, entry, arg, 0, 0, leader);
}

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
    int group = t->tgid;
    task_t *victims[MAX_TASKS];
    int n = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&scheduler_lock);
    for (int i = 0; i < task_count; i++) {
        task_t *o = &tasks[i];
        if (o == t || o->state == TASK_FREE || o->state == TASK_TERMINATED) {
            continue;
        }
        if (o->tgid == group) {
            victims[n++] = o;
        }
    }
    spin_unlock(&scheduler_lock);
    irq_restore(flags);
    for (int i = 0; i < n; i++) {
        scheduler_raise_signal(victims[i], SIGKILL);
    }
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

void scheduler_raise_signal(task_t *t, int sig) {
    if (!t || t->state == TASK_TERMINATED || t->state == TASK_FREE) {
        return;
    }
    if (sig <= 0 || sig > SIG_MAX) {
        return;
    }

    if (sig == SIGCONT) {
        t->pending_stop = 0;
        if (t->state == TASK_STOPPED) {
            scheduler_resume_stopped(t);
        }
    }

    if (!SIG_IS_CATCHABLE(sig)) {
        if (sig == SIGSTOP) {
            t->pending_stop = sig;
            scheduler_wake_task(t);
            return;
        }
        t->pending_signal = sig;
        if (t->state == TASK_STOPPED) {
            scheduler_resume_stopped(t);
        }
        scheduler_wake_task(t);
        return;
    }
    uint64_t h = t->sig_handler[sig];
    if (h == SIG_IGN_ADDR) {
        return;
    }
    if (h == SIG_DFL_ADDR) {
        switch (SIG_DEFAULT_ACTION(sig)) {
        case SIG_DFL_IGNORE:
        case SIG_DFL_CONTINUE:
            return;
        case SIG_DFL_STOP:
            t->pending_stop = sig;
            scheduler_wake_task(t);
            return;
        default:
            break;
        }
        t->pending_signal = sig;
        if (t->state == TASK_STOPPED) {
            scheduler_resume_stopped(t);
        }
        scheduler_wake_task(t);
        return;
    }
    t->sig_pending |= (1u << sig);
    scheduler_wake_task(t);
}

void scheduler_raise_signal_group(int pgid, int sig) {
    if (pgid == 0) {
        return;
    }
    for (int i = 0; i < task_count; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_FREE || t->state == TASK_TERMINATED) {
            continue;
        }
        if (t->pgid == pgid) {
            scheduler_raise_signal(t, sig);
        }
    }
}

int scheduler_signal_pending(void) {
    task_t *t = current_task[smp_current_cpu()];
    return (t->sig_pending & ~t->sig_blocked) != 0;
}

void scheduler_release_cmdline(task_t *t) {
    char *block = t->cmdline_block;
    t->cmdline_block = NULL;
    t->cmdline_length = 0;
    if (block) {
        kfree(block);
    }
}

void scheduler_set_cmdline(task_t *t, const char *const *argv) {
    scheduler_release_cmdline(t);
    if (!t || !argv || !argv[0]) {
        return;
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
        return;
    }
    char *block = (char *)kmalloc(needed);
    if (!block) {
        return;
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
    t->cmdline_block = block;
    t->cmdline_length = at;
}

/* Asked of the owner rather than of the task, because a thread has no
   command line of its own and /proc/<tid>/cmdline is defined to answer with
   the process's. */
const char *scheduler_cmdline(task_t *t, uint32_t *length) {
    task_t *owner = scheduler_vm_owner(t);
    if (!owner || !owner->cmdline_block) {
        if (length) {
            *length = 0;
        }
        return (const char *)0;
    }
    if (length) {
        *length = owner->cmdline_length;
    }
    return owner->cmdline_block;
}

void scheduler_release_env(task_t *t) {
    char *block = t->env_block;
    t->env_block = NULL;
    t->env_length = 0;
    t->env_count = 0;
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
    scheduler_release_env(t);
    t->env_block = copy;
    t->env_length = length;
    t->env_count = count;
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

void descriptor_table_reference(file_descriptor_table_t *table) {
    if (table) {
        table->references++;
    }
}

void descriptor_table_release(file_descriptor_table_t *table) {
    if (!table) {
        return;
    }
    if (--table->references > 0) {
        return;
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

void scheduler_release_file_descriptors(task_t *t) {
    /* One reference, not one pass over the slots: the descriptors close when
       the LAST task sharing this table is gone, and a thread exiting while
       its siblings run is not that. */
    descriptor_table_release(t->descriptor_table);
    t->descriptor_table = (file_descriptor_table_t *)0;
    if (flock_release_pid(t->id) > 0) {
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
}
