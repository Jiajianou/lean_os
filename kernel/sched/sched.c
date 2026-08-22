#include "sched.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "drivers/pit.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "panic.h"
#include "signal.h" /* system_api/include/signal.h - SIGKILL/SIGTERM */

#define MAX_TASKS 64 /* generous headroom for M13's shell spawning a child per typed command */
#define TASK_STACK_SIZE (8 * 1024)
#define SCHED_QUANTUM_TICKS 5 /* 5 * 10 ms PIT ticks = 50 ms time slice */

extern void context_switch(uint64_t *old_rsp_out, uint64_t new_rsp);

static task_t tasks[MAX_TASKS];
static int task_count;
static task_t *current_task;
static uint32_t ticks_in_slice;
static uint64_t loaded_pml4_phys; /* mirrors whatever schedule() last loaded into CR3, so same-address-space switches (the common case: plain kernel tasks) skip a needless TLB-flushing reload */

static void task_entry_trampoline(void) {
    task_t *t = current_task;
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

/* Runs inside IRQ0's handler on every PIT tick. Checks the *current*
 * task's pending signal first - this is what catches a task that never
 * makes a syscall (a tight compute loop, say) within one time slice,
 * since syscall_handler's own check (kernel/arch/x86_64/syscall.c) would
 * otherwise never run for it. Only actually reschedules once every
 * SCHED_QUANTUM_TICKS, so a task gets a real time slice rather than
 * being preempted on every single 10 ms tick. */
static void scheduler_tick(void) {
    if (current_task->pending_signal == SIGKILL || current_task->pending_signal == SIGTERM) {
        deliver_pending_signal_and_exit(current_task);
    }
    if (++ticks_in_slice < SCHED_QUANTUM_TICKS) {
        return;
    }
    ticks_in_slice = 0;
    schedule();
}

void sched_init(void) {
    tasks[0].state = TASK_RUNNING;
    tasks[0].id = 0;
    tasks[0].stack_base = NULL; /* this is kernel_main's own stack, not one we allocated or will ever free */
    tasks[0].kernel_stack_top = 0; /* never consulted: RSP0 only matters for a ring3->ring0 transition, and task 0 never runs in ring 3 */
    tasks[0].pml4_phys = vmm_kernel_pml4_phys();
    tasks[0].fds[0].type = FD_STDIN;
    tasks[0].fds[1].type = FD_STDOUT;
    tasks[0].parent_id = -1;
    tasks[0].pgid = 0;
    task_count = 1;
    current_task = &tasks[0];
    loaded_pml4_phys = tasks[0].pml4_phys;

    pit_set_tick_hook(scheduler_tick);
}

static task_t *task_spawn_common(uint64_t pml4_phys, void (*entry)(void *arg), void *arg) {
    if (task_count >= MAX_TASKS) {
        return NULL;
    }
    task_t *t = &tasks[task_count];
    t->id = task_count;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->pml4_phys = pml4_phys;
    t->stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!t->stack_base) {
        panic("task_spawn: out of heap memory for a task stack");
    }
    t->kernel_stack_top = (uint64_t)(t->stack_base + TASK_STACK_SIZE);

    /* Inherits the spawning task's whole fd table (so a pipe fd set up
     * before SYS_spawn carries over to the child, the same way a real
     * fork() would inherit descriptors) and process group; parent_id
     * records who to attribute this task to for SYS_wait(-1). */
    for (int i = 0; i < MAX_FDS; i++) {
        t->fds[i] = current_task->fds[i];
    }
    t->parent_id = current_task->id;
    t->pgid = current_task->pgid;
    t->pending_signal = 0;
    t->reaped = 0;

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

    task_count++;
    return t;
}

task_t *task_spawn(void (*entry)(void *arg), void *arg) {
    return task_spawn_common(vmm_kernel_pml4_phys(), entry, arg);
}

task_t *task_spawn_in(uint64_t pml4_phys, void (*entry)(void *arg), void *arg) {
    return task_spawn_common(pml4_phys, entry, arg);
}

/* Round-robin: scan forward from current, wrapping, for the next READY
 * task. TASK_RUNNING (i.e. the current task itself, if nothing else is
 * ready) and TASK_TERMINATED slots are skipped. */
static task_t *pick_next(void) {
    int start = current_task->id;
    for (int offset = 1; offset <= task_count; offset++) {
        int i = (start + offset) % task_count;
        if (tasks[i].state == TASK_READY) {
            return &tasks[i];
        }
    }
    return current_task; /* nothing else ready - keep running this one */
}

void schedule(void) {
    task_t *next = pick_next();
    if (next == current_task) {
        return;
    }

    task_t *prev = current_task;
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    current_task = next;

    tss_set_rsp0(next->kernel_stack_top);
    if (next->pml4_phys != loaded_pml4_phys) {
        vmm_switch_address_space(next->pml4_phys);
        loaded_pml4_phys = next->pml4_phys;
    }

    context_switch(&prev->rsp, next->rsp);
}

void task_exit_with_code(int code) {
    current_task->exit_code = code;
    current_task->state = TASK_TERMINATED;
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
    return current_task;
}

task_t *sched_task_by_id(int id) {
    if (id < 0 || id >= task_count) {
        return (task_t *)0;
    }
    return &tasks[id];
}

int sched_task_count(void) {
    return task_count;
}
