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

static void sched_deliver_pending_signal(void);

#define TASK_STACK_SIZE (32 * 1024)
#define SCHED_QUANTUM_TICKS 2

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);

static task_t tasks[MAX_TASKS];
static int task_count;

static task_t *current_task[MAX_CPUS];
static uint32_t ticks_in_slice[MAX_CPUS];
static uint32_t aging_ticks;

static volatile int need_resched[MAX_CPUS];

static int peak_live_tasks;
static uint64_t loaded_pml4_phys[MAX_CPUS];

static spinlock_t sched_lock;

static uint64_t event_seq;

static int blocked_count;

static task_t *pick_next(task_t *from);
void sched_dump_cpus(void);
static void wake_expired(uint64_t now_ms);
static void fire_expired_alarms(uint64_t now_ms);
static void unblock_self(task_t *self);

const int sched_poll_channel = 0;
const int sched_keyboard_channel = 0;

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

void sched_idle_enter(void) {
    uint64_t flags = irq_save_disable();
    int cpu = smp_current_cpu();
    task_t *self = current_task[cpu];
    if (self && self->idle_wait_depth < 255) {
        self->idle_wait_depth++;
    }
    irq_restore(flags);
}

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
        t->parent_id = -1;
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

static void set_task_name(task_t *t, const char *name);

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
    spin_unlock(&sched_lock);
    task_t *t = current_task[smp_current_cpu()];
    t->entry(t->arg);
    task_exit();
}

extern void fork_return_to_user(void *frame) __attribute__((noreturn));

static void fork_child_trampoline(void) __attribute__((noreturn));
static void fork_child_trampoline(void) {
    spin_unlock(&sched_lock);
    task_t *t = current_task[smp_current_cpu()];
    fork_return_to_user((void *)(t->kernel_stack_top - sizeof(isr_regs_t)));
}

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
        t->stopped_sig = 0;
        t->stop_reported = 0;
        event_seq++;
    }
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

static void take_pending_stop(task_t *t) {
    int sig = t->pending_stop;
    t->pending_stop = 0;
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    t->state = TASK_STOPPED;
    t->stopped_sig = sig;
    t->stop_reported = 0;
    event_seq++;
    spin_unlock(&sched_lock);
    irq_restore(flags);
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

void scheduler_tick_cpu(int cpu) {
    task_t *t = current_task[cpu];

    total_ticks[cpu]++;
    if (t->is_idle || sched_task_is_idle_waiting(t)) {
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

    wake_expired(pit_get_ticks() * (1000 / PIT_HZ));
    fire_expired_alarms(pit_get_ticks() * (1000 / PIT_HZ));

    if (t->pending_signal != 0) {
        deliver_pending_signal_and_exit(t);
    }
    if (t->pending_stop != 0) {
        take_pending_stop(t);
    }
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

    int preempt = need_resched[cpu] && t->prio == PRIO_BATCH;
    need_resched[cpu] = 0;
    if (!preempt && ++ticks_in_slice[cpu] < SCHED_QUANTUM_TICKS) {
        return;
    }
    ticks_in_slice[cpu] = 0;

    if (t->full_slices < 255) {
        t->full_slices++;
    }
    if (t->full_slices >= SCHED_BATCH_THRESHOLD &&
        pit_get_ticks() - t->last_block_tick > SCHED_AGING_TICKS) {
        t->prio = PRIO_BATCH;
    }
    schedule();
}

static void scheduler_tick(void) {
    scheduler_tick_cpu(smp_current_cpu());
    smp_broadcast_schedule_tick();
}

void sched_init(void) {
    tasks[0].state = TASK_RUNNING;
    tasks[0].generation = 0;
    tasks[0].id = PID_MAKE(0, 0);
    tasks[0].stack_base = NULL;
    tasks[0].kernel_stack_top = 0;
    tasks[0].pml4_phys = vmm_kernel_pml4_phys();
    fpu_state_init(tasks[0].fpu_state);
    tasks[0].fds[0].type = FD_STDIN;
    tasks[0].fds[1].type = FD_STDOUT;
    tasks[0].parent_id = -1;
    tasks[0].pgid = 0;
    tasks[0].sid = 0;
    tasks[0].caps = CAP_ALL;
    tasks[0].tgid = tasks[0].id;
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
    t->pml4_phys = vmm_kernel_pml4_phys();
    fpu_state_init(t->fpu_state);
    t->fds[0].type = FD_STDIN;
    t->fds[1].type = FD_STDOUT;
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
    t->env_len = 0;
    t->env_count = 0;
    set_task_name(t, "cpu-idle");
    current_task[cpu_id] = t;
    loaded_pml4_phys[cpu_id] = t->pml4_phys;
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

static task_t *task_spawn_common(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                                  uint64_t heap_start, uint64_t shm_base, task_t *thread_of) {
    _Static_assert(TASK_STACK_SIZE % 4096 == 0, "a kernel stack must be a whole number of frames");
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
    task_t *caller = current_task[smp_current_cpu()];
    t->id = PID_MAKE(slot, t->generation);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->pml4_phys = pml4_phys;
    t->stack_base = stack_base;
    t->kernel_stack_top = (uint64_t)(stack_base + TASK_STACK_SIZE);

    for (int i = 0; i < MAX_FDS; i++) {
        t->fds[i] = caller->fds[i];
        if (t->fds[i].cloexec) {
            t->fds[i].type = FD_NONE;
            t->fds[i].cloexec = 0;
            continue;
        }
        fd_retain(&t->fds[i]);
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
    t->max_rss_pages = 0;
    t->child_max_rss_pages = 0;
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
    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = SIG_DFL_ADDR;
    }
    t->sig_restorer = 0;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_siginfo = 0;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_addr = 0;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i].base = 0;
        t->mmaps[i].pages = 0;
        t->mmaps[i].memfd_id = 0;
        t->mmaps[i].memfd_gen = 0;
    }
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
    t->shm_next_vaddr = shm_base;
    set_task_name(t, name);

    uint64_t *sp = (uint64_t *)t->kernel_stack_top;
    *(--sp) = (uint64_t)task_entry_trampoline;
    *(--sp) = 0x202;
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

    spin_unlock(&sched_lock);
    irq_restore(flags);
    return t;
}

task_t *task_spawn(const char *name, void (*entry)(void *arg), void *arg) {
    return task_spawn_common(name, vmm_kernel_pml4_phys(), entry, arg, 0, 0, (task_t *)0);
}

task_t *task_spawn_in(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                       uint64_t heap_start, uint64_t shm_base) {
    return task_spawn_common(name, pml4_phys, entry, arg, heap_start, shm_base, (task_t *)0);
}

static int current_on_some_cpu(const task_t *t) {
    for (int c = 0; c < MAX_CPUS; c++) {
        if (current_task[c] == t) {
            return 1;
        }
    }
    return 0;
}

static task_t *pick_next(task_t *from) {
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
            tasks[i].prio = PRIO_INTERACTIVE;
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

int sched_wake_n(const void *chan, int max) {
    if (!chan || max <= 0) {
        return 0;
    }
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    event_seq++;
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
    event_seq++;
    if (blocked_count == 0) {
        spin_unlock(&sched_lock);
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
    spin_unlock(&sched_lock);
    irq_restore(flags);
}

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
    sched_deliver_pending_signal();
    sched_take_pending_stop_if_any(current_task[smp_current_cpu()]);

    int cpu = smp_current_cpu();
    uint64_t sflags = irq_save_disable();
    spin_lock(&sched_lock);
    if (event_seq != expected_seq) {
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

    sched_deliver_pending_signal();

    schedule();
    unblock_self(self);
    *flags = spin_lock_irqsave(lock);
}

void schedule(void) {
    int cpu = smp_current_cpu();
    uint64_t flags = irq_save_disable();
    spin_lock(&sched_lock);
    task_t *prev = current_task[cpu];
    task_t *next = pick_next(prev);

    if (next == prev) {
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return;
    }

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

    fpu_save(prev->fpu_state);
    fpu_restore(next->fpu_state);

    cpu_write_msr(MSR_FS_BASE, next->fs_base);

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

    context_switch(&prev->rsp, next->rsp);

    spin_unlock(&sched_lock);
    irq_restore(flags);
}

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

static void sched_deliver_pending_signal(void) {
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
    shm_free_by_owner(t->id);
    sched_release_fds(t);
    sched_release_env(t);

    if (t->sid != 0 && t->sid == t->id) {
        tty_release_session(tty_console(), t->sid);
        pty_release_session(t->sid);
    }

    if (t->pml4_phys != vmm_kernel_pml4_phys()) {
        int cpu = smp_current_cpu();
        uint64_t dead = t->pml4_phys;
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
        blocked_count--;
    }
    t->state = TASK_TERMINATED;
    sched_wake_all((const void *)t);
    sched_wake_all(SCHED_POLL_CHAN);
    if (t->parent_id >= 0) {
        task_t *parent = sched_task_by_id(t->parent_id);
        if (parent && parent != t) {
            parent->si_pid = (int32_t)t->id;
            parent->si_status = t->exit_signal ? (int32_t)(t->exit_signal & 0x7F)
                                               : (int32_t)(t->exit_code & 0xFF);
            sched_raise_signal(parent, SIGCHLD);
        }
    }
    schedule();
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
    uint64_t flags;
    {
        uint64_t deadline = pit_get_ticks() + PIT_HZ;
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
                break;
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
    task_t *parent = sched_task_by_id(t->parent_id);
    if (parent) {
        if (t->is_thread && t->tgid == parent->tgid) {
            parent->user_ticks += t->user_ticks;
            parent->sys_ticks += t->sys_ticks;
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
    t->max_rss_pages = 0;
    t->child_max_rss_pages = 0;
    t->exit_signal = 0;
    t->exit_code = 0;
    t->prio = PRIO_INTERACTIVE;
    t->full_slices = 0;
    t->parent_id = -1;
    t->caps = 0;
    sched_reset_fds_to_std(t);
    t->env_block = NULL;
    t->env_len = 0;
    t->env_count = 0;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_restorer = 0;
    t->sig_siginfo = 0;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_addr = 0;
    for (int i = 0; i <= SIG_MAX; i++) {
        t->sig_handler[i] = SIG_DFL_ADDR;
    }
    sched_regions_forget_memfds(t);
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i].base = 0;
        t->mmaps[i].pages = 0;
    }
    t->fs_base = 0;
    t->is_thread = 0;
    t->exiting = 0;
    t->tgid = 0;
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    t->fds[0].type = FD_NONE;
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
    return leader ? leader : t;
}

#define FILL_REFUSE ((uint64_t)-1)

static uint64_t fill_policy(task_t *self, uint64_t page, int for_write, int for_exec) {
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
            break;
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
    if ((region->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) {
        return FILL_REFUSE;
    }
    if (for_write && !(region->prot & PROT_WRITE)) {
        return FILL_REFUSE;
    }
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

void sched_region_forget_memfd(mmap_region_t *r) {
    if (!r || !r->memfd_id) {
        return;
    }
    struct memfd *m = memfd_by_tag((uint8_t)(r->memfd_id - 1), r->memfd_gen);
    r->memfd_id = 0;
    r->memfd_gen = 0;
    memfd_region_unref(m);
}

void sched_regions_forget_memfds(task_t *t) {
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        sched_region_forget_memfd(&t->mmaps[i]);
    }
}

void sched_regions_retain_memfds(task_t *t) {
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0 || !t->mmaps[i].memfd_id) {
            continue;
        }
        struct memfd *m = memfd_by_tag((uint8_t)(t->mmaps[i].memfd_id - 1),
                                       t->mmaps[i].memfd_gen);
        if (!m) {
            t->mmaps[i].memfd_id = 0;
            t->mmaps[i].memfd_gen = 0;
            continue;
        }
        memfd_region_ref(m);
    }
}

int sched_release_shared_range(task_t *t, uint64_t start, uint64_t end) {
    int dropped = 0;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
        }
        if (t->mmaps[i].memfd_id) {
            uint64_t rstart = t->mmaps[i].base;
            uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
            uint64_t from = start > rstart ? start : rstart;
            uint64_t to = end < rend ? end : rend;
            for (uint64_t p = from; p < to; p += PAGE_SIZE) {
                if (!vmm_user_range_ok(t->pml4_phys, p, 1, 0)) {
                    continue;
                }
                vmm_unmap_page_in(t->pml4_phys, p);
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
            if (!vmm_user_range_ok(t->pml4_phys, p, 1, 0)) {
                continue;
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

    if (vmm_user_range_ok(self->pml4_phys, page, 1, 0)) {
        return 0;
    }

    uint64_t flags = fill_policy(self, page, for_write, for_exec);
    if (flags == FILL_REFUSE) {
        return 0;
    }

    const mmap_region_t *region = mmap_region_for(self, page);
    if (region && region->memfd_id) {
        struct memfd *m = memfd_by_tag((uint8_t)(region->memfd_id - 1), region->memfd_gen);
        if (!m) {
            return 0;
        }
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        uint64_t phys = memfd_frame(m, index);
        if (phys == 0) {
            return 0;
        }
        return vmm_try_map_page_in(self->pml4_phys, page, phys, flags) == 0
                   ? 1
                   : FILL_NO_MEMORY;
    }
    if (region && region->handle >= 0 && region->shared) {
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        uint64_t sphys = filemap_get(region->handle, index,
                                     (region->prot & PROT_WRITE) != 0);
        if (sphys == 0) {
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
        return FILL_NO_MEMORY;
    }
    k_memset((void *)phys, 0, PAGE_SIZE);
    if (region && region->handle >= 0) {
        uint32_t index = region->file_page +
                         (uint32_t)((page - region->base) / PAGE_SIZE);
        (void)vfs_handle_read(region->handle, (void *)phys, PAGE_SIZE,
                              index * PAGE_SIZE);
    }
    if (vmm_try_map_page_in(self->pml4_phys, page, phys, flags) != 0) {
        pmm_free_frame(phys);
        return FILL_NO_MEMORY;
    }
    return 1;
}

static int fill_one_page(task_t *self, uint64_t page, int for_write) {
    return fill_one_page_ex(self, page, for_write, 0);
}

#define STACK_GROW_SLACK 65536ULL

int sched_fault_fill(uint64_t addr, uint64_t error_code, uint64_t user_rsp) {
    task_t *self = sched_vm_owner(sched_current());
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return 0;
    }
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);

    if (page >= USER_STACK_LIMIT && page < USER_STACK_TOP) {
        if (addr + STACK_GROW_SLACK < user_rsp) {
            return 0;
        }
    }

    if (error_code & 1u) {
        if (error_code & 2u) {
            return vmm_cow_break(self->pml4_phys, page);
        }
        return 0;
    }
    return fill_one_page_ex(self, page, (error_code & 2u) != 0, (error_code & 16u) != 0);
}

void sched_prefault_range(uint64_t addr, uint64_t len, int for_write) {
    if (len == 0) {
        return;
    }
    task_t *self = sched_vm_owner(sched_current());
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return;
    }
    if (vmm_user_range_ok(self->pml4_phys, addr, len, for_write)) {
        return;
    }
    uint64_t first = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (addr + len - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (last < first) {
        return;
    }
    int in_arena = !(last < USER_MMAP_BASE || first >= USER_MMAP_LIMIT);
    int in_stack = !(last < USER_STACK_LIMIT || first >= USER_STACK_TOP);
    if (!in_arena && !in_stack) {
        return;
    }
    for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
        if (vmm_user_range_ok(self->pml4_phys, page, 1, for_write)) {
            continue;
        }
        if (for_write && vmm_cow_break(self->pml4_phys, page)) {
            continue;
        }
        fill_one_page(self, page, for_write);
    }
}

task_t *task_fork(uint64_t child_pml4, const isr_regs_t *regs) {
    _Static_assert(TASK_STACK_SIZE % 4096 == 0, "a kernel stack must be a whole number of frames");
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
    t->entry = NULL;
    t->arg = NULL;
    t->state = TASK_READY;
    t->pml4_phys = child_pml4;
    t->stack_base = stack_base;
    t->kernel_stack_top = (uint64_t)(stack_base + TASK_STACK_SIZE);

    for (int i = 0; i < MAX_FDS; i++) {
        t->fds[i] = parent->fds[i];
        fd_retain(&t->fds[i]);
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
    t->max_rss_pages = 0;
    t->child_max_rss_pages = 0;

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
    t->sig_pending = 0;
    t->sig_siginfo = parent->sig_siginfo;
    t->si_pid = 0;
    t->si_status = 0;
    t->si_addr = 0;

    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        t->mmaps[i] = parent->mmaps[i];
    }
    sched_regions_retain_memfds(t);

    t->tgid = t->id;
    t->is_thread = 0;
    t->exiting = 0;
    t->caps = parent->caps;

    for (size_t i = 0; i < sizeof(t->fpu_state); i++) {
        t->fpu_state[i] = parent->fpu_state[i];
    }

    t->fs_base = parent->fs_base;

    t->heap_brk = parent->heap_brk;
    t->heap_mapped_end = parent->heap_mapped_end;
    t->shm_next_vaddr = parent->shm_next_vaddr;
    set_task_name(t, parent->name);

    isr_regs_t *child_frame =
        (isr_regs_t *)(t->kernel_stack_top - sizeof(isr_regs_t));
    *child_frame = *regs;
    child_frame->rax = 0;

    uint64_t *sp = (uint64_t *)child_frame;
    *(--sp) = (uint64_t)fork_child_trampoline;
    *(--sp) = 0x202;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    t->rsp = (uint64_t)sp;

    spin_unlock(&sched_lock);
    irq_restore(flags);
    return t;
}

task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg) {
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

    if (sig == SIGCONT) {
        t->pending_stop = 0;
        if (t->state == TASK_STOPPED) {
            sched_resume_stopped(t);
        }
    }

    if (!SIG_IS_CATCHABLE(sig)) {
        if (sig == SIGSTOP) {
            t->pending_stop = sig;
            sched_wake_task(t);
            return;
        }
        t->pending_signal = sig;
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
        switch (SIG_DEFAULT_ACTION(sig)) {
        case SIG_DFL_IGNORE:
        case SIG_DFL_CONTINUE:
            return;
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
    sched_wake_task(t);
}

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

void fd_release(fd_slot_t *slot) {
    slot->cloexec = 0;
    slot->nonblock = 0;
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
    case FD_UNIX:
        unixsock_unref(slot->un);
        break;
    case FD_EVENT:
        eventfd_unref(slot->event);
        break;
    case FD_TIMER:
        timerfd_unref(slot->timer);
        break;
    case FD_EPOLL:
        epoll_unref(slot->epoll);
        break;
    case FD_MEMFD:
        memfd_unref(slot->memfd);
        break;
    default:
        break;
    }
    slot->type = FD_NONE;
    slot->pipe = (struct pipe *)0;
}

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
    case FD_UNIX:
        unixsock_ref(slot->un);
        break;
    case FD_EVENT:
        eventfd_ref(slot->event);
        break;
    case FD_TIMER:
        timerfd_ref(slot->timer);
        break;
    case FD_EPOLL:
        epoll_ref(slot->epoll);
        break;
    case FD_MEMFD:
        memfd_ref(slot->memfd);
        break;
    default:
        break;
    }
}

void sched_release_fds(task_t *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_release(&t->fds[i]);
    }
    if (flock_release_pid(t->id) > 0) {
        sched_wake_all(FLOCK_CHAN);
    }
}

void sched_reset_fds_to_std(task_t *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        fd_release(&t->fds[i]);
    }
    t->fds[0].type = FD_STDIN;
    t->fds[1].type = FD_STDOUT;
}
