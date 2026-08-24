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

#include "proc.h" /* system_api/include/proc.h - TASK_INFO_MAX, which *is* MAX_TASKS below. Resolves to the system_api header: a quoted include searches this file's own directory first (kernel/sched/, no proc.h), then -Ikernel (no kernel/proc.h), then -Isystem_api/include. */

struct pipe; /* kernel/ipc/pipe.h owns the real definition - not included here so sched.h doesn't have to know pipes exist */

/* M54: TASK_FREE is 0 so a zeroed table is a table of free slots, which
 * is what makes slot recycling a property of the array rather than of a
 * separate in-use bitmap somebody has to keep in step. Before this
 * milestone there was no such state: a slot was used from the moment it
 * was first handed out until the machine was switched off. */
typedef enum {
    TASK_FREE = 0,
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

/* M21: bumped from 8 - a compositor juggling several windows needs
 * stdin/stdout plus a request/response pipe pair plus one event-pipe
 * write end per connected client, well past 8 (same "bump the fixed cap
 * when a real need arrives" precedent as MAX_TASKS's M13 8->64).
 *
 * M40: bumped again, 32 -> 64, after 32 turned out to be an *exact* fit
 * rather than headroom - and an exact fit that silently broke the
 * desktop. The compositor needs 2 (stdin/stdout) + 12 (its six protocol
 * pipes, two fd slots each) + 2 per connected window; at MAX_WINDOWS (8)
 * that is precisely 30 of 32, leaving no room for anything at all being
 * inherited from its parent - which is exactly what was happening (see
 * sched_reset_fds_to_std). Nothing here should be load-bearing to the
 * last slot: 64 leaves the same 8-window compositor half its table
 * spare.
 *
 * M50: bumped again, 64 -> 128, and this time from a logged measurement
 * rather than an argument. The boot self-test's own "[m50] compositor
 * after the storm" line reports what a compositor that has been through
 * sixteen connect/draw/die rounds actually holds, and it was 52 of 64.
 * The derivation behind that number:
 *
 *    2  stdin/stdout
 *  +22  eleven well-known protocol pipes, two fd slots each (window
 *       create/response, query/response, action, settings,
 *       settings-query/response, notify, drag, drag-data)
 *  +24  two per window slot ever created, and slots are recycled but
 *       their event pipes are kept and reset rather than reopened -
 *       2 x WM_MAX_ROUTABLE_WINDOWS
 *  = 48 for a compositor that has filled every window slot once
 *
 * 12 spare of 64 is not headroom, it is the state M40 was in when this
 * broke the first time - and two more protocol channels or a larger
 * WM_MAX_ROUTABLE_WINDOWS would have taken it. 128 leaves the same
 * compositor 80 slots clear. (M50 also gave clients five of their own
 * back: wm_connect closes the handshake pipes it used to hold forever -
 * see wmclient.c.) */
#define MAX_FDS 128

/* M45: how long a per-task name may be, NUL included. A process was a
 * number and nothing else until this milestone - only whoever spawned it
 * knew what it was, which made SYS_taskinfo's enumeration (and the task
 * manager built on it) unable to show anything a person could act on.
 * 24 comfortably holds every program name this project ships (the
 * longest, "desktop_icons"/"gui_terminal", are 13) and leanfs's own
 * LEANFS_MAX_NAME (27 + NUL) is the real upper bound on what a spawn
 * path can even be; a name longer than this is truncated, never a spawn
 * failure - a display string is not worth refusing to run a program
 * over. */
#define TASK_NAME_MAX 24

/* M45: moved out of sched.c, where it had lived since M13. A caller that
 * enumerates the task table (kernel.c's own self-tests, and SYS_taskinfo
 * behind them) has to size a buffer for it, and a second hand-picked
 * number that merely happened to be >= this is exactly the kind of
 * near-duplicate cap this project has been bitten by twice (M40 and M41
 * both shipped a bug that was really an exactly-sized cap).
 *
 * M48: 64 -> 128, and this time from a measurement rather than a guess -
 * the third time this project has shipped a bug that was really an
 * exactly-sized cap, and the first time the machine said so itself. The
 * seventh app launched from a desktop icon began failing, and because
 * M48 had just given SYS_spawn distinct error codes the desktop put
 * "Too many programs are running." on screen instead of doing nothing.
 *
 * The derivation, from the boot log's own "[sched] task table at
 * handoff" line (kernel.c, added for exactly this):
 *
 *   54  slots the boot self-tests have already spent by the time PID 1
 *       starts - they are never given back, because ids are assigned
 *       sequentially and slots are never recycled
 *  + 4  init, the compositor, desktop_icons and desktop_shell
 *  +10  the most app windows that can exist at once
 *       (WM_MAX_ROUTABLE_WINDOWS, 12, minus the desktop and the panel)
 *  = 68 for a desktop with everything open, once
 *
 * 128 is that plus 60 more launches, which is the part that actually
 * matters: closing an app does *not* return its slot, so the real budget
 * is "how many programs may be started over this machine's whole
 * uptime", and 68 would have been another exact fit. */
/* M54: 128 stays, but it finally means what it says. Until this
 * milestone slots were never recycled, so this was a budget for
 * *launches over the machine's whole uptime* - the boot self-tests alone
 * spent 79 of it before PID 1 started, and closing an app never gave one
 * back. Now that a reaped task's slot returns, the same number is a
 * ceiling on how many tasks may exist *at once*, which is the thing a
 * fixed table should be sizing. The measured live count on a fully
 * loaded desktop is under twenty. */
#define MAX_TASKS 128

/* ---- M54: pids are (slot, generation) ---------------------------------
 *
 * A task id used to be a slot index, which worked only because slots were
 * never reused. Recycling them without changing that would silently make
 * SYS_wait, SYS_wait_nb and SYS_task_alive answer about *the wrong
 * process* - the single most dangerous shape of bug this change could
 * introduce, and one no test would obviously catch, since every answer
 * would look plausible.
 *
 * So a pid packs both into the same `int` every existing caller already
 * passes around: the slot in the low PID_SLOT_BITS, and a generation
 * counter above it that increments each time the slot is handed out
 * again. A stale pid names a slot whose generation has moved on, and is
 * reported as invalid rather than as somebody else.
 *
 * 8 slot bits covers MAX_TASKS with room to grow; the remaining 22 usable
 * bits of a positive int give four million reuses per slot, which at this
 * machine's spawn rate is not a wrap anyone will see. Task 0 is
 * (slot 0, generation 0) == pid 0, which is what keeps kernel_main's own
 * identity unchanged. */
#define PID_SLOT_BITS 8
#define PID_SLOT_MASK ((1 << PID_SLOT_BITS) - 1)
#define PID_MAKE(slot, gen) (((gen) << PID_SLOT_BITS) | (slot))
#define PID_SLOT(pid) ((pid) & PID_SLOT_MASK)
#define PID_GEN(pid) (((unsigned)(pid)) >> PID_SLOT_BITS)

typedef struct {
    fd_type_t type;
    struct pipe *pipe;
} fd_slot_t;

typedef struct task {
    uint64_t rsp; /* saved stack pointer while not running; meaningless while this is the current task */
    uint8_t *stack_base;
    uint64_t kernel_stack_top; /* fixed; installed as TSS.RSP0 whenever this task is about to run (M9) */
    task_state_t state;
    int id; /* M54: a pid, not an index - see PID_MAKE. PID_SLOT(id) is where in this table it lives. */
    /* M54: bumped every time this slot is handed to a new task, so a pid
     * held by somebody who kept it too long can be told from a live one.
     * Never reset. */
    unsigned generation;
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
    char name[TASK_NAME_MAX]; /* M45: what this task was spawned as - the path process_spawn loaded, or a short label for a kernel thread. Always NUL-terminated. Display only: nothing looks a task up by name, and two tasks may freely share one. */
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
task_t *task_spawn(const char *name, void (*entry)(void *arg), void *arg);

/* Like task_spawn, but the task's first action (via `entry`) is expected
 * to drop to ring 3 into its own private address space - see
 * kernel/proc/proc.c's process_spawn, which is the actual entry point
 * user code should use; this is the lower-level primitive it's built on. */
/* M40: whether a task_spawn* call could succeed right now, without
 * allocating anything to find out. Advisory - task_spawn_common re-checks
 * under sched_lock, which is the authoritative one - but it lets
 * process_spawn bail before building an address space it would have to
 * tear down again.
 *
 * M54: this now genuinely means "is there room for one more task", where
 * it used to mean "has this machine spawned MAX_TASKS tasks *ever*". */
int sched_has_free_task_slot(void);

/* M40: heap_start/shm_base are the new task's SYS_sbrk and SYS_shm_map
 * starting cursors (proc.h's USER_HEAP_START/USER_SHM_BASE - the
 * scheduler deliberately doesn't know those constants, it just stores
 * what proc.c hands it). Passed in rather than assigned by the caller
 * afterwards because this task becomes schedulable - on any CPU - the
 * moment task_spawn_common's lock drops; see that function's own comment
 * for the panic that came of getting this wrong. */
/* M45: `name` (may be NULL - stored as an empty string) is copied in
 * under the same lock that publishes this task as schedulable, for
 * exactly the reason heap_start/shm_base are: the task is runnable the
 * instant the lock drops, and SYS_taskinfo running on another CPU must
 * never see a half-written one. */
task_t *task_spawn_in(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                       uint64_t heap_start, uint64_t shm_base);

/* Picks the next READY task (round-robin from the current one) and
 * context-switches to it if it isn't the current task. Safe to call
 * voluntarily (cooperative yield) or from interrupt context (preemption). */
void schedule(void);

/* Marks the current task TERMINATED and switches away from it forever -
 * never returns. Called automatically when a task's entry function
 * returns (with code 0); tasks may also call task_exit_with_code
 * directly (SYS_exit does) to exit early with a real status. */
/* M42: terminates the calling task *now* if it has a fatal signal
 * pending, and returns otherwise.
 *
 * sys_kill's contract (kernel/arch/x86_64/syscall.c) is that a signal is
 * noticed "at the target's next syscall entry or scheduler tick". Both
 * checkpoints miss the same case, and it is not a rare one: a task
 * blocked *inside* a syscall - pipe_read with nothing buffered, SYS_read
 * on the console with no keystrokes, SYS_wait on a child - is already
 * past the syscall-entry check, and is spinning on schedule() from inside
 * an interrupt gate with IF clear, so no timer tick ever fires while it
 * is the current task either. Such a task could not be killed at all: the
 * signal sat pending forever and SYS_wait on it never returned. Found by
 * making the boot self-tests actually wait for what they kill (M42), at
 * which point the first client that blocks on its event pipe rather than
 * polling it - gui_paint.c - hung the whole boot.
 *
 * So every blocking loop in a syscall calls this alongside its
 * schedule(). Not noreturn: it returns normally in the overwhelmingly
 * common case of no signal pending. */
void sched_deliver_pending_signal(void);

void task_exit(void) __attribute__((noreturn));
void task_exit_with_code(int code) __attribute__((noreturn));

task_t *sched_current(void);

/* Lookup by pid. Returns NULL for a slot that is free, out of range, or
 * whose generation has moved on - which is what makes a *stale* pid an
 * error rather than an answer about whoever holds that slot now (M54).
 *
 * A terminated task stays valid and inspectable until it is reaped, which
 * is what SYS_wait's poll-the-exit-code approach needs. Reaping is also
 * what frees the slot: see sched_reap_slot. */
task_t *sched_task_by_id(int pid);

/* M54: enumeration by *slot*, for the callers that walk the whole table
 * looking for something (SYS_wait(-1) finding children, SYS_taskinfo,
 * the shutdown path signalling everyone). Returns NULL for a free slot.
 * Split from sched_task_by_id because the two questions stopped being
 * the same one the moment a pid stopped being an index - and conflating
 * them is exactly how a recycled slot would be mistaken for its previous
 * occupant. */
task_t *sched_task_by_slot(int slot);

/* The exclusive upper bound for sched_task_by_slot - the high-water mark
 * of slots ever handed out, not a count of live tasks. Recycling means a
 * slot below this may well be free; sched_task_by_slot says so. */
int sched_task_count(void);

/* M54: how many slots are currently occupied. What "[sched] task table at
 * handoff" reports now, and the number MAX_TASKS is a ceiling on. */
int sched_live_task_count(void);

/* M54: releases a terminated task's slot back to the table - its kernel
 * stack freed and its generation bumped, so its pid can never be
 * confused with whatever lands there next.
 *
 * Called from exactly one place in spirit and three in code: whenever a
 * task's exit status has been *consumed* (SYS_wait, SYS_wait_nb, and
 * SYS_wait(-1)'s reap of an arbitrary child). Deliberately not on exit:
 * a terminated-but-unreaped task is precisely what the compositor's
 * SYS_task_alive polling reads to tell an orderly exit from a crash, and
 * recycling under it would turn "this client died" into "this pid is
 * unknown" - a distinction M29's reap_dead_clients and M48's crash toast
 * both depend on. So an unwaited task still holds a slot, exactly as
 * before; the population that stopped consuming the table is the one that
 * was always reaped and never gave anything back - the boot self-tests. */
void sched_reap_slot(task_t *t);

/* M40: drops every fd this task holds except stdin/stdout, restoring the
 * table a freshly-spawned process would have started with.
 *
 * This exists for exactly one caller: kernel_main, right before it spawns
 * PID 1. Boot-time self-tests (M14's SYS_pipe round trip, and M30/M33/
 * M36/M38's kernel-side SYS_pipe_open calls onto WM_ACTION_PIPE /
 * WM_SETTINGS_PIPE) open pipes from task 0 and never close them - there
 * is no SYS_close in this project, deliberately (see pipe.h). Harmless
 * on its own, except that a spawned task inherits its parent's *whole*
 * fd table (task_spawn_common) and every user process descends from task
 * 0, so those five leftover fd pairs rode all the way into the
 * compositor and cost it ten of its own slots. With MAX_FDS at 32 that
 * left room for exactly two windows past the desktop background and the
 * panel: launching a third app from a desktop icon got a failed
 * sys_pipe_open, a window_id of -1, and no window - the "double-clicking
 * the Editor/Clock icons does nothing" bug M40 was opened on, which was
 * never about those two programs at all (it was whichever two you
 * launched third and fourth). See milestones.md's M40 section.
 *
 * Not a general-purpose close: nothing here reference-counts a pipe, so
 * this only makes sense for a task that is about to stop using its fds
 * entirely, which task 0 (the idle task from here on) is. */
void sched_reset_fds_to_std(task_t *t);
