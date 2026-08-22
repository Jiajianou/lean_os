/* kernel/sched/sched.h
 *
 * Kernel-thread round-robin scheduler. Every task shares the kernel's
 * single address space (there's no per-process page table yet - that's
 * M9) and gets preempted every SCHED_QUANTUM_TICKS PIT ticks
 * (drivers/pit.h's tick hook, see sched_init). Preemption reuses the
 * interrupt machinery M4 already built: a tick fires inside a normal IRQ0
 * interrupt, and context_switch (context_switch.asm) swaps which kernel
 * stack is "current" out from under that same interrupt frame - see
 * context_switch.asm's header comment for exactly how that's safe.
 */
#pragma once

#include <stdint.h>

struct pipe; /* kernel/ipc/pipe.h owns the real definition - not included here so sched.h doesn't have to know pipes exist */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_TERMINATED,
} task_state_t;

/* M14: a minimal per-task descriptor table. FD_STDIN/FD_STDOUT are
 * fixed, implicit slots every task gets (fd 0/1); FD_PIPE_READ/
 * FD_PIPE_WRITE point at a kernel/ipc/pipe.h object. No real files-on-
 * disk fds (SYS_readfile/SYS_listfiles are whole-shot, no fd needed). */
typedef enum {
    FD_NONE = 0,
    FD_STDIN,
    FD_STDOUT,
    FD_PIPE_READ,
    FD_PIPE_WRITE,
} fd_type_t;

#define MAX_FDS 32 /* M21: bumped from 8 - a compositor juggling several windows needs stdin/stdout plus a request/response pipe pair plus one event-pipe write end per connected client, well past 8 (same "bump the fixed cap when a real need arrives" precedent as MAX_TASKS's M13 8->64). */

typedef struct {
    fd_type_t type;
    struct pipe *pipe;
} fd_slot_t;

typedef struct task {
    uint64_t rsp; /* saved stack pointer while not running; meaningless while this is the current task */
    uint8_t *stack_base;
    uint64_t kernel_stack_top; /* fixed; installed as TSS.RSP0 whenever this task is about to run (M9) */
    task_state_t state;
    int id;
    void (*entry)(void *arg);
    void *arg;
    uint64_t pml4_phys; /* this task's address space - the shared kernel one for a plain kernel thread, a private one (M9) for a user process */
    int exit_code; /* valid once state == TASK_TERMINATED; set by task_exit_with_code (M13) */
    fd_slot_t fds[MAX_FDS]; /* fd 0/1 default to FD_STDIN/FD_STDOUT; a spawned task inherits its parent's whole table (M14) so pipe fds set up before SYS_spawn carry over */
    int parent_id; /* -1 for task 0 (nothing spawned it) */
    int pgid; /* process group: a process's own id if it's a group leader, otherwise inherited from whoever spawned it - read-only (SYS_getpgid), no job control to ever need changing it yet */
    int pending_signal; /* 0 = none, else SIGKILL/SIGTERM (system_api/include/signal.h) - checked at the next syscall entry or scheduler tick, see syscall.c/sched.c */
    int reaped; /* SYS_wait(-1) sets this once it's returned this task's id, so a later wait(-1) call doesn't hand back the same dead child twice */
    /* M19: per-process virtual memory bookkeeping (proc.h's USER_HEAP_
     * and USER_SHM_BASE constants) - meaningless (left zeroed) for a plain
     * kernel thread spawned via task_spawn rather than process_spawn,
     * since only a ring-3 process can ever reach the syscalls that read
     * them (SYS_sbrk, SYS_shm_map). */
    uint64_t heap_brk;        /* current break - SYS_sbrk's return/growth point */
    uint64_t heap_mapped_end; /* one past the last vmm-mapped heap page; heap_brk <= this always */
    uint64_t shm_next_vaddr;  /* next free address for this process's own SYS_shm_map calls */
} task_t;

/* Turns the currently executing context (kernel_main, at whatever point
 * it calls this) into task 0 and installs the PIT tick hook. Must run
 * before task_spawn. */
void sched_init(void);

/* SMP: the AP-side equivalent of sched_init - turns the calling AP's own
 * boot context (kernel/arch/x86_64/smp.c's ap_main) into a new task in
 * the same shared table sched_init's task 0 lives in, so this core has a
 * "current task" identity for schedule()/pick_next to work with. Every
 * CPU pulls from the same run queue - there's no per-CPU task affinity -
 * so this is really just "register one more idle-loop task", not a
 * separate scheduler instance. */
void sched_init_ap(int cpu_id);

/* Runs the same per-tick bookkeeping scheduler_tick (sched.c, static) does
 * for the BSP's real PIT interrupt, but for an arbitrary CPU - called
 * directly for cpu 0 from the PIT path, and via IPI_SCHEDULE_VECTOR
 * (kernel/arch/x86_64/smp.c's lapic_vector_handler) for every other online
 * CPU, since the 8259 only ever delivers the real timer interrupt to the
 * BSP. */
void scheduler_tick_cpu(int cpu);

/* Allocates a kernel stack and a task_t, marks it READY, and adds it to
 * the round-robin rotation. Runs in the kernel's own (shared) address
 * space. Returns NULL if the fixed task table is full. */
task_t *task_spawn(void (*entry)(void *arg), void *arg);

/* Like task_spawn, but the task's first action (via `entry`) is expected
 * to drop to ring 3 into its own private address space - see
 * kernel/proc/proc.c's process_spawn, which is the actual entry point
 * user code should use; this is the lower-level primitive it's built on. */
task_t *task_spawn_in(uint64_t pml4_phys, void (*entry)(void *arg), void *arg);

/* Picks the next READY task (round-robin from the current one) and
 * context-switches to it if it isn't the current task. Safe to call
 * voluntarily (cooperative yield) or from interrupt context (preemption). */
void schedule(void);

/* Marks the current task TERMINATED and switches away from it forever -
 * never returns. Called automatically when a task's entry function
 * returns (with code 0); tasks may also call task_exit_with_code
 * directly (SYS_exit does) to exit early with a real status. */
void task_exit(void) __attribute__((noreturn));
void task_exit_with_code(int code) __attribute__((noreturn));

task_t *sched_current(void);

/* Bounds-checked lookup by id (== index - ids are assigned sequentially
 * and slots are never recycled, see task_spawn_common). Returns NULL for
 * an out-of-range id. Terminated tasks stay valid and inspectable
 * forever (their slot is never freed/reused) - that's what makes
 * SYS_wait's poll-the-exit_code approach work without any separate
 * zombie/reap bookkeeping. */
task_t *sched_task_by_id(int id);

/* Total number of tasks ever spawned (== the exclusive upper bound of
 * valid ids) - lets callers (SYS_wait(-1), M14) enumerate every task to
 * find children without needing their own separate child-list
 * bookkeeping. */
int sched_task_count(void);
