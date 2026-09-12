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

#include "caps.h" /* system_api/include/caps.h - CAP_*, M65 */
#include "signal.h" /* system_api/include/signal.h - SIG_MAX, the per-task handler table below, M76 */
#include "paths.h" /* system_api/include/paths.h - PATH_MAX_LEN, M75's cwd */
#include "lib/spinlock.h"
#include "arch/x86_64/fpu.h" /* M63: FPU_STATE_SIZE/ALIGN - a task carries its own SSE state now */

#include "arch/x86_64/isr.h" /* M83: isr_regs_t, the frame task_fork copies */
#include "proc.h" /* system_api/include/proc.h - TASK_INFO_MAX, which *is* MAX_TASKS below. Resolves to the system_api header: a quoted include searches this file's own directory first (kernel/sched/, no proc.h), then -Ikernel (no kernel/proc.h), then -Isystem_api/include. */

struct pipe; /* kernel/ipc/pipe.h owns the real definition - not included here so sched.h doesn't have to know pipes exist */
struct eventfd;  /* M119: kernel/ipc/eventfd.h, timerfd.h and epoll.h - forward-declared for the same reason as the two above */
struct timerfd;
struct epoll;
struct unixsock; /* M118: kernel/ipc/unixsock.h owns it, and that header includes THIS one - so the forward declaration is load-bearing, not tidy */

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
    /* M68: the state the other four have been faking since M7. A blocked
     * task is not runnable - pick_next skips it - and stays that way
     * until something calls sched_wake_all on the channel it parked on,
     * or its deadline passes. Every "blocking" call in this kernel used
     * to be a loop calling schedule(), which left the task READY forever:
     * a shell sitting at a prompt was, as far as the scheduler could
     * tell, a program that wanted the CPU as much as any other. That is
     * why no core on this machine had ever halted while a desktop was
     * running. */
    TASK_BLOCKED,
    /* M85: stopped by a job-control signal, and waiting for SIGCONT.
     *
     * Deliberately its own state rather than a flag on TASK_BLOCKED, and
     * the difference is the one that matters: a blocked task is waiting
     * for something that will happen on its own - bytes, a deadline, a
     * child - and any wake makes it runnable again. A stopped task is
     * waiting for a *decision*, and nothing but SIGCONT (or SIGKILL) may
     * make it runnable. Making it a blocked task with a flag would mean
     * every sched_wake_all in the kernel had to remember not to wake it,
     * which is fourteen places that have to keep remembering; making it a
     * state means pick_next's existing "is it TASK_READY" test excludes
     * it for free and always will. */
    TASK_STOPPED,
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
    /* M59: a file on disk, finally. Points at an entry in the kernel's
     * shared open-file table (kernel/fs/openfile.h) rather than carrying
     * the inode and offset inline, for the reason every real OS does the
     * same: two fds produced by SYS_dup2 have to share one offset, or
     * "append to the same file through both" quietly interleaves. */
    FD_FILE,
    /* M64: a UDP socket (kernel/net/socket.h). Refcounted exactly like
     * the other two, which is the whole reason a socket is an fd here
     * and not its own handle namespace. */
    FD_SOCKET,
    /* M118: a Unix-domain socket (kernel/ipc/unixsock.h), which is a
     * different kind from FD_SOCKET rather than a flag on it. They share
     * a name in <sys/socket.h> and nothing else: one is a port on an
     * interface, the other is a buffer between two processes on this
     * machine. Keeping them apart is what lets AF_UNIX need no
     * capability while AF_INET needs CAP_NETWORK - see unixsock.h for
     * why that difference is the point rather than an inconsistency. */
    FD_UNIX,
    /* M119: the three objects a message pump is made of
     * (kernel/ipc/eventfd.h, timerfd.h, epoll.h). Three kinds rather than
     * one "pollable" kind with a tag inside it, because read() means
     * something different on each - a counter, an expiration count, and
     * nothing at all - and a single type would put that switch somewhere
     * less obvious than here. */
    FD_EVENT,
    FD_TIMER,
    FD_EPOLL,
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

/* ---- M78 ------------------------------------------------------------
 *
 * How many separate anonymous mappings one process may hold at once. A
 * program that maps a few large arenas needs a handful; malloc as it is
 * written here uses one per large allocation. 32 is comfortably past
 * either and costs 512 bytes a task. Past it, SYS_mmap returns -1 -
 * which is a thing a caller can report, unlike the alternative of
 * silently merging two mappings a munmap would then have to guess about.
 *
 * M91: 32 -> 128. Three things this milestone added all consume slots
 * rather than pages. mprotect splits a region in two or three every time
 * it is applied to part of one - which is what a dynamic loader does to
 * every object it maps, making its relocation table read-only after
 * fixing it up. MAP_FIXED replaces a subrange, which splits as well.
 * And PROT_NONE guard pages are regions that hold no memory at all and a
 * slot each. A program that maps twenty shared objects and protects three
 * ranges in each is past 32 before it has run a line of its own code.
 * 128 costs 1.5 KiB a task, which against MAX_TASKS is a fifth of a
 * megabyte - a real cost, and a smaller one than a loader that fails at
 * the twelfth library.
 */
#define MAX_MMAP_REGIONS 128

typedef struct {
    uint64_t base;  /* page-aligned start; meaningless when pages == 0 */
    uint32_t pages; /* 0 = this slot is free */
    /* M82: the PROT_* the caller asked for, kept because the mapping is
     * no longer built at the moment it is asked for. A page in this
     * region gets its table entry when it is first touched, and the fault
     * that touches it has to know whether the caller was promised
     * writable memory - which is a fact about the *mapping* and therefore
     * has to live with the mapping rather than in a page table entry that
     * does not exist yet. */
    uint32_t prot;
    /* ---- M91 (second attempt): what backs this mapping ----------------
     *
     * `handle` is the vfs handle of the file behind it, or -1 for
     * anonymous memory - which is every mapping this kernel could make
     * until now. `file_page` is which page of that file the region's
     * first page is; the fault handler adds the offset within the region
     * to it.
     *
     * `shared` says whether the frames are this address space's own or
     * everybody's. It changes three things and each is somewhere else:
     * the fault fills from kernel/mm/filemap.c rather than from a fresh
     * zeroed frame; fork gives the child the same frame rather than a
     * copy-on-write one; and unmap gives the frame back to filemap
     * rather than to the pmm, because somebody else may still be
     * holding it. */
    int handle;
    uint32_t file_page;
    uint8_t shared;
} mmap_region_t;

/* M45: how long a per-task name may be, NUL included. A process was a
 * number and nothing else until this milestone - only whoever spawned it
 * knew what it was, which made SYS_taskinfo's enumeration (and the task
 * manager built on it) unable to show anything a person could act on.
 * 24 comfortably holds every program name this project ships (the
 * longest, "desktop_icons"/"gui_terminal", are 13); a name longer than
 * this is truncated, never a spawn failure - a display string is not
 * worth refusing to run a program over.
 *
 * M81: this used to add "and leanfs's own LEANFS_MAX_NAME (27 + NUL) is
 * the real upper bound on what a spawn path can even be", which stopped
 * being true when names went to 255. Removed rather than updated,
 * because the second clause was never the reason for the number - it was
 * a reassurance that the first clause could not be wrong by much, and
 * now that it can be, the honest version is the first clause alone plus
 * the truncation rule. */
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
    /* M59: a union rather than a second pointer field. task_t holds
     * MAX_FDS of these and there are MAX_TASKS tasks, so eight more bytes
     * here is 128 KiB of kernel BSS for a field only one of the two
     * descriptor kinds ever uses. They are mutually exclusive by
     * construction - `type` says which. */
    union {
        struct pipe *pipe;
        struct openfile *file;
        struct socket *sock;
        struct unixsock *un; /* M118 */
        struct eventfd *event;  /* M119 */
        struct timerfd *timer;
        struct epoll *epoll;
    };
    /* M84: FD_CLOEXEC, and it finally means something.
     *
     * <fcntl.h> defined this as a bit nothing checked, with an honest
     * note that "there is no exec on this machine (SYS_spawn loads a
     * fresh image and copies the fd table), so FD_CLOEXEC has nothing to
     * mean and is genuinely not set". SYS_execve is what gives it
     * something to mean: an exec keeps the fd table, so a descriptor
     * that should not survive into a different program needs a way to
     * say so.
     *
     * A whole byte for one bit, and deliberately: MAX_FDS * MAX_TASKS is
     * 16384 slots, so this is 16 KiB of BSS against the alternative of
     * packing it into `type` - which would make every existing read of
     * `type` a masked read and every write a read-modify-write, for
     * sixteen kilobytes on a machine with a hundred and twenty-eight. */
    uint8_t cloexec;
    /* M100: O_NONBLOCK, for a socket or a pipe. The same byte-for-a-bit
     * trade as cloexec above and for the same reason. Read by SYS_read
     * and SYS_write (which return -OS_ERR_AGAIN rather than parking) and
     * by nothing else - SYS_recv and SYS_send never blocked to begin
     * with, and SYS_waitfds is a wait by definition. */
    uint8_t nonblock;
} fd_slot_t;

/* ---- M69: two scheduling classes, third attempt and the one that works --
 *
 * The first two attempts are written up below and both failed the same
 * way, from opposite directions: whatever signal was used, **the
 * compositor was misclassified**, because it never blocked. It
 * busy-polled its request pipes and ended its loop in SYS_yield, so a
 * yield-based classifier called everything interactive and a wake-based
 * one demoted the compositor and let the clients waiting on it win.
 *
 * What changed is not the classifier. It is that the compositor now
 * *blocks* - its main loop ends in SYS_waitfds (M68's call, adopted here)
 * rather than in a spin. So the signal below finally describes it
 * correctly, and it describes everything else correctly too:
 *
 *   **a task that was TASK_BLOCKED and got woken is waiting on the
 *   world**, and should run the moment it can. A task that burns whole
 *   slices without ever blocking is computing.
 *
 * A spinning task cannot claim the first however often it polls, and a
 * blocking one cannot be denied it - which is what makes this immune to
 * both inversions rather than to one of them.
 *
 * Demotion needs 10 consecutive whole slices (100 ms of uninterrupted
 * CPU), well clear of the largest burst of honest work an interactive
 * task does in one go - a compositor frame is ~16 ms. Aging returns
 * everything to interactive once a second, so "a batch task always makes
 * progress" is true by construction rather than by argument.
 */
typedef enum {
    PRIO_INTERACTIVE = 0, /* was blocked and got woken - runs first */
    PRIO_BATCH = 1,       /* burned whole slices without ever blocking */
} prio_class_t;

#define SCHED_BATCH_THRESHOLD 10  /* 100 ms of uninterrupted CPU */
#define SCHED_AGING_TICKS     100 /* 1 s: nothing stays batch longer than this */

/* ---- M69 (first attempt): why there were no scheduling priorities -----
 *
 * This milestone set out to add two classes - interactive and batch,
 * separated by the classic heuristic that a task which gives the CPU up
 * before its slice is done is waiting for something and should run first.
 * It was implemented, measured, and removed, because **the signal does
 * not exist on this machine yet.**
 *
 * Every process on this desktop busy-polls. The compositor's main loop
 * ends in SYS_yield; every wmclient program's does too; a blocking
 * sys_read on a pipe is a spin through schedule(). So *everything* looks
 * like it yields voluntarily, all the time, and a classifier fed that
 * signal marks the entire machine interactive - which is the same as
 * marking none of it.
 *
 * What it actually produced was a priority inversion that wedged the
 * boot. `wm_demo` waits for its window in a blocking sys_read, spinning
 * and re-declaring itself interactive on every pass. The compositor does
 * real work - a frame takes more than one 10 ms slice - so it burned
 * whole slices and was demoted. The client then outranked the server it
 * was waiting for: a handshake that takes milliseconds had not finished
 * after fifteen seconds. Raising the demotion threshold from 2 slices to
 * 10 did not fix it, it only moved the victim - `kernel_main`, which
 * halts in pit_sleep_ms and so also never "yields", was starved instead,
 * and a `pit_sleep_ms(10)` started taking 100 ms.
 *
 * The conclusion is not "priorities are hard", it is specific and
 * actionable: **a scheduler cannot tell waiting from computing until
 * waiting is a thing a task can actually do.** That is M68. The ordering
 * this file argued about after M68's first attempt turns out to be
 * genuinely circular - M68 needed M69's measurement to be verifiable, and
 * M69's classifier needs M68's blocked state to have anything to
 * classify - and the resolution is that only the *measurement* half of
 * M69 was ever the prerequisite. That half is done, so M68 can be
 * attempted again with numbers, and priorities become possible for the
 * first time immediately after it.
 *
 * What M69 keeps is the part that needs no classifier at all: a quantum
 * of one tick instead of five, which cut the loaded case by more than
 * half on its own.
 */

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
    /* M84: 0 if this task exited of its own accord, otherwise the signal
     * that killed it.
     *
     * The exit code alone cannot answer this. A task killed by SIGSEGV
     * exits with 128 + 11, and so does a program that calls exit(139) -
     * the shell convention that makes the two *readable* is exactly what
     * makes them indistinguishable to a program. SYS_wait has squashed
     * them together since M13 and SYS_task_alive's own comment documents
     * the squash; waitpid is the call that has to tell them apart, so
     * this is the field that remembers which happened. */
    int exit_signal;
    /* M85: a stop that has been raised but not yet taken.
     *
     * Separate from pending_signal, whose invariant is documented at its
     * one delivery point and is worth keeping: "nonzero means a signal
     * whose disposition on this task is death". A stop is not a death and
     * folding it in would make that sentence false everywhere it is
     * relied on. Taken at the same two places pending_signal is - the
     * scheduler tick and the syscall boundary - because a stop, like a
     * death, is something that happens to a task at a moment it is not
     * in the middle of something. */
    int pending_stop;
    /* M85 (second attempt): which signal actually stopped this task, and
     * whether a waiting parent has been told.
     *
     * `pending_stop` is a *request* and is cleared the moment it is
     * taken; a parent calling waitpid(WUNTRACED) needs the fact that the
     * stop happened, which outlives the request. And it needs it exactly
     * once - a shell that printed "[1]+ Stopped" on every loop of its
     * wait would print it forever, because the child stays stopped. */
    int stopped_sig;
    uint8_t stop_reported;
    /* M63: this task's x87/SSE registers while it is not running. 512
     * bytes and 16-byte aligned, both architectural requirements of
     * FXSAVE rather than preferences - see arch/x86_64/fpu.h for why
     * only a *task switch* has to touch this and an interrupt does not. */
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));
    fd_slot_t fds[MAX_FDS]; /* fd 0/1 default to FD_STDIN/FD_STDOUT; a spawned task inherits its parent's whole table (M14) so pipe fds set up before SYS_spawn carry over */
    int parent_id; /* -1 for task 0 (nothing spawned it) */
    /* M65: what this process is allowed to do (system_api/include/caps.h).
     * Monotonically non-increasing for the life of the task: a child is
     * given a subset at spawn, a process can drop its own, and there is
     * no code path anywhere that sets a bit that was clear. That single
     * property is what makes the model checkable rather than merely
     * present - it means nothing has to be trusted to hand one back. */
    uint32_t caps;
    /* Process group: a process's own id if it is a group leader,
     * otherwise inherited from whoever spawned it. Read-only until M85 -
     * "no job control to ever need changing it yet" was the note, and
     * SYS_setpgid is what job control needed. */
    int pgid;
    /* M85: the session this process belongs to.
     *
     * A session is a set of process groups that share a controlling
     * terminal, and it is the level at which "which program is the
     * terminal talking to" is decided - the group is which *job*, the
     * session is which *terminal*. Inherited like pgid; changed only by
     * setsid, which makes a process the leader of a brand-new session
     * with no terminal at all. That last part is the whole reason a
     * daemon calls it. */
    int sid;
    /* ---- M96: the thread pointer -------------------------------------
     *
     * What IA32_FS_BASE holds while this task runs, so that `%fs:0` -
     * which is what every access to a `__thread` variable goes through -
     * points at this thread's own block and not at somebody else's.
     *
     * Per TASK rather than per address space, which is the whole point:
     * two threads share every page and must not share this. It is
     * restored on every context switch beside the FPU state and for the
     * same reason - a task that resumed with the previous task's thread
     * pointer would read the previous task's errno.
     *
     * 0 means "never set", and a task with 0 gets 0 written to the MSR
     * rather than being skipped: skipping would leave whatever the
     * outgoing task had, which is the exact bug this field exists to
     * prevent. */
    uint64_t fs_base;
    int pending_signal; /* 0 = none, else SIGKILL/SIGTERM (system_api/include/signal.h) - checked at the next syscall entry or scheduler tick, see syscall.c/sched.c */
    /* M89: when this task's alarm(2) expires, in ms since boot; 0 for no
     * alarm. Separate from wake_deadline_ms, which is a sleep: a sleep
     * ends by making a blocked task runnable, an alarm interrupts a
     * running one with a signal, and a task can have both. */
    uint64_t alarm_deadline_ms;
    int reaped; /* SYS_wait(-1) sets this once it's returned this task's id, so a later wait(-1) call doesn't hand back the same dead child twice */
    /* M19: per-process virtual memory bookkeeping (proc.h's USER_HEAP_
     * and USER_SHM_BASE constants) - meaningless (left zeroed) for a plain
     * kernel thread spawned via task_spawn rather than process_spawn,
     * since only a ring-3 process can ever reach the syscalls that read
     * them (SYS_sbrk, SYS_shm_map). */
    uint64_t heap_brk;        /* current break - SYS_sbrk's return/growth point */
    uint64_t heap_mapped_end; /* one past the last vmm-mapped heap page; heap_brk <= this always */
    uint64_t shm_next_vaddr;  /* next free address for this process's own SYS_shm_map calls */
    /* ---- M68: what this task is waiting for ---------------------------
     *
     * `wait_chan` is an address used purely as an identity - a pipe_t*, a
     * task_t*, or the address of one of the well-known channels in
     * sched.h. Nothing dereferences it. This is V6 Unix's sleep/wakeup
     * and xv6's after it, and it is the right shape here for the same
     * reason it was there: the task table is fixed and small, so "wake
     * everyone waiting on X" is a 128-entry scan and needs no per-channel
     * list, no allocation, and nothing to leak.
     *
     * `wake_deadline_ms` is an absolute uptime, or 0 for "no deadline".
     * The timer tick wakes anything past it, which is what makes a
     * blocking wait safe to use for something that might never be woken
     * and what lets one call serve as both a wait and a sleep. */
    const void *wait_chan;
    uint64_t wake_deadline_ms;
    /* M69: scheduler-owned. Nothing outside sched.c writes them and no
     * syscall can ask for a class - a flag a program sets is a flag every
     * program sets. */
    uint8_t prio;
    uint8_t full_slices;
    /* M69: when this task last came out of TASK_BLOCKED, in PIT ticks.
     * A task that blocked recently is waiting-driven even if it happens
     * to be spinning right now - which the compositor does deliberately
     * while an animation is running, to meet a 16 ms frame budget a
     * 10 ms timer cannot. Without this it demoted itself during exactly
     * the 140 ms when it most needed the CPU, and M61's animation
     * self-test caught it. Still a signal a spinner cannot fake: it has
     * to have actually blocked to set it. */
    uint64_t last_block_tick;
    /* M68: this task is a CPU's idle identity - task 0 after it reaches
     * kernel_main's tail, or an AP's `cpu-idle`. Marked explicitly rather
     * than inferred from "no stack and no parent", which happened to be
     * true of exactly these two and is the kind of coincidence that stops
     * being true the first time somebody adds a third parentless kernel
     * thread (kernel/net/net.c's tcp-timer is already one). Used only to
     * decide whether a tick counts as idle time. */
    uint8_t is_idle;
    /* M88: where this task's CPU time went, in PIT ticks, split by the
     * privilege level the timer interrupted. `times()` and `getrusage()`
     * are the first callers this machine has ever had for either number,
     * and until this milestone nothing anywhere counted them - the task
     * manager's percentages were derived from a CPU's idle counter, so
     * "how much of that was this process" had no answer at all.
     *
     * Ticks rather than microseconds because a tick is what this machine
     * actually observes: the counter is incremented by the timer, so a
     * finer unit would be arithmetic on a number that was never measured.
     * _SC_CLK_TCK reports the same 100 Hz, which is what makes a clock_t
     * from times() mean something on the receiving side.
     *
     * The child counters hold what has been REAPED, which is what
     * `times()` specifies and is also the only version that can be
     * correct: an unreaped child's time is still accruing. */
    uint64_t user_ticks;
    uint64_t sys_ticks;
    uint64_t child_user_ticks;
    uint64_t child_sys_ticks;
    /* M98: the high-water mark of this task's resident set, in pages,
     * and the largest one any child it has reaped ever reached.
     *
     * The live number lives in the VMM, keyed on the address space (see
     * vmm_rss_peak_pages) - which is right, because an address space is
     * what has a resident set. These two exist because an address space
     * does not outlive the process: exec throws one away and exit frees
     * it, so the peak has to be *taken off* the address space at both of
     * those points or the number is gone before anyone can ask for it.
     *
     * child_max_rss_pages is a maximum rather than a sum, and that is
     * what getrusage(RUSAGE_CHILDREN)'s ru_maxrss means everywhere: the
     * biggest a child ever got, which is the number that decides whether
     * a machine can host a build. Summing would answer a question - "how
     * much memory did the whole build use at once" - that is false for
     * any two children that did not overlap. */
    uint64_t max_rss_pages;
    uint64_t child_max_rss_pages;
    /* M101: how deep this TASK is inside a sched_idle_enter/exit bracket
     * - a task halted waiting for the clock, which pit_sleep_ms does.
     *
     * There was a per-CPU idle_depth doing M68's accounting, and the
     * profiler tried to use it first. It could not: the bracket in
     * pit_sleep_ms wraps a `hlt` loop that the timer interrupt can
     * schedule *away* from, so the next task to run on that CPU finds
     * the counter still raised and looks idle while it is working. That
     * mistake classified an entire kernel busy loop as idle time and was
     * caught by this milestone's own self-test. The counter that answers
     * "is this task waiting" has to live on the task.
     *
     * M106 deleted the per-CPU counter outright and made M68's own idle
     * accounting ask this one instead. On four cores it had a second and
     * worse failure: enter and exit ran on different CPUs whenever a task
     * migrated while it slept, so the count leaked upward on one core
     * until every tick it took was recorded as idle. A counter owned by
     * the thing it describes cannot desynchronise from it. */
    uint8_t idle_wait_depth;
    /* ---- M75: a place to stand, and something to stand there with -----
     *
     * `cwd` is this task's working directory - always absolute, always
     * normalized, never with a trailing slash unless it IS "/". Every
     * path-taking syscall resolves against it (syscall.c's
     * resolve_user_path), which is what makes a relative name mean
     * something on a machine whose filesystem deliberately stores
     * neither "." nor "..".
     *
     * `env_block` is this task's environment, packed as back-to-back
     * NUL-terminated "NAME=value" strings, with `env_count` of them in
     * `env_len` bytes. NULL for a task that has none (every kernel
     * thread, and any process spawned before something put one there).
     *
     * Kernel-side rather than "read the child's own argument page when
     * it spawns": a process may write over that page, and an environment
     * that a program can corrupt for its *children* is a footgun with no
     * upside. This copy is what a child inherits. It is kmalloc'd, owned
     * by exactly this slot, and released in task_exit_with_code beside
     * the fd table - the other thing a task holds that outlives its last
     * instruction. */
    char cwd[PATH_MAX_LEN];
    char *env_block;
    uint32_t env_len;
    uint32_t env_count;
    /* ---- M76: signals a program can catch ----------------------------
     *
     * `pending_signal` above is still what it always was: the *fatal*
     * one, taken at the next syscall entry or scheduler tick, which ends
     * the task. These four are the other kind.
     *
     * `sig_handler[n]` is SIG_DFL_ADDR, SIG_IGN_ADDR, or a ring-3
     * function address. `sig_restorer` is where a handler `ret`s to -
     * supplied by user space at sigaction time rather than assumed,
     * because the kernel has no business knowing where a program links
     * its runtime. `sig_pending` and `sig_blocked` are bitmasks over
     * signal numbers; one bit, so a second SIGINT arriving before the
     * first is handled is the same bit, which is what every Unix does
     * for non-realtime signals and is why they are not a queue.
     *
     * None of it is inherited across a spawn. A handler address belongs
     * to the image that installed it, and a fresh image has never seen
     * it - carrying one over would point ring 3 at somebody else's
     * program. task_spawn_common zeroes all four for that reason. */
    uint64_t sig_handler[SIG_MAX + 1];
    uint64_t sig_restorer;
    uint32_t sig_pending;
    uint32_t sig_blocked;
    /* ---- M99: SA_SIGINFO, and the facts a siginfo_t carries ----------
     *
     * `sig_siginfo` is one bit per signal: the handler for it was
     * installed with SA_SIGINFO and therefore expects THREE arguments,
     * not one. That distinction is a calling convention, and getting it
     * wrong is not a missing feature - it is a program reading %rsi and
     * %rdx as pointers when nothing put anything in them.
     *
     * It was wrong here. <signal.h> said "a handler installed with
     * SA_SIGINFO is called through sa_handler with the signal number ...
     * the pointer arguments are never passed", which is an accurate
     * description of an unsafe thing to do: toybox's `timeout` installs
     * exactly such a handler for SIGCHLD and its first line is
     * `si->si_status`. It faulted at address 5 - offsetof(si_status)
     * added to whatever happened to be in %rsi - every single time,
     * which is how M99 found it.
     *
     * The three scratch fields are what the kernel knows at the moment
     * it raises a signal and cannot reconstruct later: which child ended
     * and how (SIGCHLD), and which address faulted (SIGSEGV/SIGBUS).
     * One set rather than one per signal, and that is a documented
     * imprecision rather than an oversight: signals here are bits, not a
     * queue (see above), so two children ending before the parent runs
     * are already one SIGCHLD, and this reports the second. Every Unix
     * without real-time signals has the same property. */
    uint32_t sig_siginfo;
    int32_t  si_pid;    /* SIGCHLD: which child */
    int32_t  si_status; /* SIGCHLD: its exit status, in wait()'s encoding */
    uint64_t si_addr;   /* SIGSEGV/SIGBUS: the address that faulted */
    /* ---- M78: the mmap arena's bookkeeping ---------------------------
     *
     * One entry per live anonymous mapping, kept sorted by base address
     * with `pages == 0` marking a free slot. Sorted because that is what
     * makes "find the first gap big enough" a single forward scan - and
     * finding a *gap* rather than a high-water mark is the entire point
     * of this milestone. A bump allocator has no concept of a hole; an
     * ordered list is the smallest thing that does.
     *
     * A fixed array rather than a linked structure for the same reason
     * every table in this kernel is fixed: there is nothing to allocate
     * and nothing to leak, and MAX_MMAP_REGIONS entries is 512 bytes a
     * task. The cost is a ceiling on how many *separate* mappings one
     * process may hold, which is a number a program can be told rather
     * than a failure it cannot diagnose (SYS_mmap returns -1). */
    mmap_region_t mmaps[MAX_MMAP_REGIONS];
    /* ---- M79: threads ------------------------------------------------
     *
     * `tgid` is the thread group's id - this task's own for anything
     * that is not a thread, which is every task that existed before this
     * milestone. `is_thread` says whether this task was created by
     * SYS_thread_create rather than by loading an image.
     *
     * Both exist because "a task IS a process" was true here for
     * seventy-eight milestones and stopped being true in one commit.
     * Everything that had quietly relied on it - the address space being
     * this task's to tear down, the sbrk break being this task's to
     * move, "my pid" - had to be told which of the two it actually
     * meant. sched_vm_owner is where that answer lives for the memory
     * half, and it is a plain function rather than a rule call sites
     * follow, because a rule a call site can get wrong is a rule.
     *
     * `exiting` is set the instant a task commits to leaving, under
     * sched_lock, and is what makes "am I the last user of this address
     * space" answerable without two threads exiting at once each seeing
     * the other as alive and neither freeing anything. */
    int tgid;
    uint8_t is_thread;
    uint8_t exiting;
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

/* ---- M68: sleep and wakeup ---------------------------------------------
 *
 * The contract, and it is the whole of the lost-wakeup problem:
 *
 *   THE CALLER MUST HOLD THE LOCK THAT GUARDS THE CONDITION, and pass it
 *   in. sched_block_on releases it only after this task is already marked
 *   BLOCKED, and reacquires it before returning.
 *
 * That ordering is what makes the check and the block one step as far as
 * any waker is concerned. A waker has to hold the same lock to change the
 * condition, so it cannot run between "the pipe is empty" and "I am
 * blocked" - the two states a lost wakeup needs to slip between. After
 * the lock is dropped a waker may indeed set this task READY before it
 * ever reaches schedule(); that is harmless, and the task simply gets
 * scheduled again.
 *
 * Callers must re-test their condition in a loop after this returns.
 * A return means "something may have changed", never "what you wanted
 * happened" - the deadline may have passed, or another waiter may have
 * taken the bytes first.
 *
 * deadline_ms is an absolute uptime (pit_uptime_ms() + N), or 0 for none.
 */
void sched_block_on(const void *chan, uint64_t deadline_ms, spinlock_t *lock, uint64_t *flags);

/* Marks every task blocked on `chan` READY. Safe to call from an
 * interrupt handler and from a task holding any lock: it touches only the
 * task table, under sched_lock, and never blocks. Waking a channel nobody
 * is on is free and is the common case. */
void sched_wake_all(const void *chan);

/* M96: the same, bounded. Returns how many were woken - which is what
 * FUTEX_WAKE reports and what makes "wake one" mean one rather than
 * everybody. See the implementation for why sched_wake_all was not
 * rewritten in terms of it. */
int sched_wake_n(const void *chan, int max);

/* ---- M68: the sequence counter, and the race it closes ------------------
 *
 * sched_block_on's contract works because the caller holds the lock that
 * guards its condition, so no waker can run between the test and the
 * park. Three callers have no such lock, and cannot have one:
 *
 *   - SYS_waitfds, whose condition is "any of these eight descriptors",
 *     guarded by four different locks in three subsystems;
 *   - SYS_read on stdin, whose condition is a ring buffer written by an
 *     interrupt handler;
 *   - the mouse, which is the sharp case, because it has no descriptor at
 *     all - the compositor reads it through SYS_mouse_read, so it cannot
 *     be named in a waitfds set and its only wake is the IRQ.
 *
 * For those, an event arriving between the test and the park is a wake
 * delivered to a task that is not yet asleep - and then the task sleeps,
 * with the event already waiting for it. On the compositor that is a
 * mouse movement the cursor does not follow until the next timeout: not a
 * hang, but visible lag, and exactly the kind of "sometimes the pointer
 * sticks" bug that is miserable to find later.
 *
 * The fix is the standard one. Every wake bumps a counter. A lockless
 * waiter samples it *before* testing its condition, and hands the sample
 * back when it parks; if the counter has moved since, something happened
 * during the test and the waiter loops instead of sleeping. Sample and
 * park are both taken under sched_lock, which is what makes the
 * comparison meaningful.
 *
 * Wrapping is not a concern: it is 64 bits at a few thousand events per
 * second. */
uint64_t sched_event_seq(void);

/* Like sched_block_on, but for a caller with no condition lock. Does not
 * block at all if the event counter has moved since `expected_seq` was
 * sampled. */
void sched_block_on_seq(const void *chan, uint64_t deadline_ms, uint64_t expected_seq);

/* M68: the one channel every poller shares. A task waiting on several
 * descriptors at once cannot record several channels, so all of them park
 * here and any interesting event wakes the lot. With fewer than twenty
 * processes on this machine the thundering herd costs a scan and a few
 * needless wakeups, which is a great deal cheaper than the per-object
 * waiter lists the alternative needs - and every waiter re-tests its own
 * condition on the way out anyway, which is the loop the contract above
 * already requires. */
extern const int sched_poll_channel;
#define SCHED_POLL_CHAN (&sched_poll_channel)

/* M68: raw keystrokes. Woken by the keyboard IRQ, waited on by a
 * SYS_read that found the ring buffer empty. */
extern const int sched_keyboard_channel;
#define SCHED_SLEEP_CHAN (&sched_sleep_channel)
extern const int sched_sleep_channel;
#define SCHED_KEYBOARD_CHAN (&sched_keyboard_channel)

/* M68: how many timer ticks this CPU has spent with nothing to run.
 * The number this milestone exists to move off zero - see the [m68]
 * self-test in kernel.c, which is the only honest way to tell a machine
 * that sleeps from one that spins, since the two are indistinguishable
 * from the outside. */
/* M68: the calling task is this CPU's idle identity from here on.
 * kernel_main calls it once it reaches its `for(;;) hlt` tail - before
 * that, task 0 is doing the entire boot and its ticks are not idle. */
/* M68: spawn `cpus` idle tasks - somewhere for schedule() to go when a
 * task blocks or dies and there is no other work. Called once, after
 * sched_init, before anything can block. See sched.c for what happened
 * without them. */
void sched_spawn_idle_tasks(int cpus);

void sched_mark_self_idle(void);

/* M68: bracket a stretch in which the calling task is contributing
 * nothing - it is halted, waiting for time to pass. pit_sleep_ms is the
 * one such stretch in this kernel, and without this the boot's own
 * measurement is a lie in the *pessimistic* direction: the CPU spends
 * most of the boot inside `hlt` in pit_sleep_ms, genuinely idle, on a
 * task 0 that is not the idle identity yet because it has not finished
 * booting. Counting those ticks as busy would have the machine report
 * zero idle time at precisely the moments it is doing the least.
 *
 * A counter rather than a flag: pit_sleep_ms is not reentrant today, but
 * "this nests" is cheaper to guarantee than to remember. */
/* M68: leave the run queue until `deadline_ms` (an absolute uptime).
 *
 * The sleep primitive pit_sleep_ms should always have been built on. That
 * function has spent sixty-seven milestones halting in a loop while
 * remaining TASK_READY, which means every task in a timed wait - and
 * kernel/net's tcp-timer thread spends its entire life in one - was still
 * in the run queue the whole time. It is why the idle measurement read
 * zero even after everything else in M68 was working: there was always
 * exactly one runnable task, so no tick ever qualified as idle.
 *
 * No sequence check and no channel anyone wakes: the only thing that ends
 * this wait is the clock, so there is no event to race with. */
void sched_sleep_until(uint64_t deadline_ms);

void sched_idle_enter(void);
void sched_idle_exit(void);

/* M101: is `t` inside a sched_idle_enter/exit bracket right now - that
 * is, halted waiting for the clock rather than doing work?
 *
 * The profiler needs it and nothing else does yet. A task halted in
 * pit_sleep_ms is in ring 0 at an address inside pit_sleep_ms, so a
 * sampler that only looks at CS records it as kernel time in a hot
 * function. The first profile this machine ever produced said exactly
 * that: half of "kernel time" was `pit_sleep_ms+0x50`, which is the
 * `hlt` instruction.
 *
 * That is the failure kernel/profile/sampler.c's own comment warns about
 * one paragraph before it happened - "a profile that learns to lie about
 * an idle machine". The task's `is_idle` flag does not catch it either:
 * that marks the idle *task*, and this is an ordinary task waiting. */
int sched_task_is_idle_waiting(const task_t *t);

/* M68 debugging aid: every task, its state, and what it is parked on.
 * A blocked machine says nothing on its own - this is what turns "the
 * boot stopped" into "these three tasks are waiting on these channels". */
void sched_debug_dump(const char *label);

uint64_t sched_idle_ticks(int cpu);
uint64_t sched_total_ticks(int cpu);

/* M88: charge one timer tick to whatever this CPU is currently running.
 * `user` is the privilege level the timer interrupted, which is the only
 * place that distinction is available - by the time the scheduler hook
 * runs, the interrupted frame is gone.
 *
 * Called from the interrupt handlers themselves (pit.c for the BSP,
 * smp.c's lapic_vector_handler for every other core) rather than from
 * scheduler_tick_cpu, because that function takes a CPU index and not an
 * interrupt frame - and a tick that ends in a context switch never comes
 * back to be accounted afterwards. Same reason profile_sample sits where
 * it does. */
void sched_account_tick(int user);

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

/* M75: replaces `t`'s environment with a copy of `block` - `count`
 * back-to-back NUL-terminated strings occupying `len` bytes. Frees
 * whatever was there. `block` may be NULL/empty, which clears it.
 * Returns 0, or -1 if there was no heap for the copy (in which case the
 * task keeps the environment it had). */
int sched_set_env(task_t *t, const char *block, uint32_t len, uint32_t count);

/* M84: rename a task that already exists. execve is the one caller - the
 * name a person sees should be the program actually running, not the one
 * that was originally spawned into this pid. */
void sched_set_task_name(task_t *t, const char *name);

/* M84: terminate the current task because of a signal, recording which.
 * The exit code is still 128 + sig - every shell in the world reads that
 * convention and nothing here changes it - but the signal is remembered
 * separately so waitpid can report a death as a death rather than as a
 * number that happens to look like one. */
void task_exit_with_signal(int sig) __attribute__((noreturn));

/* M75: releases `t`'s environment block. Called from task_exit_with_code;
 * exposed because sched_reap_slot has to be able to assert it is gone. */
void sched_release_env(task_t *t);

/* ---- M76 -------------------------------------------------------------
 *
 * Posts `sig` to `t`, applying the *target's* disposition rather than
 * the sender's intent - which is the whole reason this is one function
 * and not a field assignment at each call site. Four outcomes:
 *
 *   SIGKILL, or any signal with no handler and no default but death:
 *       t->pending_signal, i.e. the existing fatal path, unchanged
 *   SIG_IGN_ADDR, or SIGCHLD's default:
 *       dropped
 *   a handler:
 *       a bit in t->sig_pending, delivered on the way back to ring 3
 *
 * A blocked target is woken, because a signal that arrives while a
 * process is parked and is not seen until the next unrelated event is a
 * signal that did not arrive.
 *
 * Safe to call from a task other than `t`, and from task_exit_with_code
 * (SIGCHLD). Not safe from an interrupt handler: it takes sched_lock. */
/* M89: alarm(2). Sets this task's one alarm to `seconds` from now (0
 * cancels) and returns how many whole seconds were left on the previous
 * one, rounded up. See fire_expired_alarms in sched.c for the
 * resolution and for why there is exactly one per task. */
unsigned int sched_set_alarm(task_t *t, unsigned int seconds);

/* M91 (second attempt): drop this task's references to the shared file
 * pages in [start, end), unmapping each as it goes. See the
 * implementation for why only mapped pages count. Returns how many. */
int sched_release_shared_range(task_t *t, uint64_t start, uint64_t end);

void sched_raise_signal(task_t *t, int sig);

/* 1 if the *current* task has a caught signal waiting that is not
 * blocked. The blocking calls in syscall.c consult it so that a park can
 * end for a signal rather than only for the event it was waiting on -
 * without which a handler installed by a program that spends its life in
 * SYS_waitfds would never run. */
int sched_signal_pending(void);

/* Wakes one specific task, whatever channel it is parked on. sched_wake_all
 * is by channel, which is right for "the pipe you were waiting on has
 * bytes" and wrong for "something happened to *you*". */
void sched_wake_task(task_t *t);

/* M85: put a task stopped by a job-control signal back on the run queue.
 * Deliberately not sched_wake_task, which only ever moves TASK_BLOCKED:
 * every wake in this kernel goes through that function and none of them
 * should be able to resume a suspended process by accident. */
void sched_resume_stopped(task_t *t);

/* M85: take a pending job-control stop, if this task has one.
 *
 * Exported so the syscall boundary can call it - see the note at the
 * call site in syscall_dispatch for why one check site was not enough,
 * and take_pending_stop for what taking one does. Returns with the task
 * still running if there was nothing to take. */
void sched_take_pending_stop_if_any(task_t *t);

/* M85: raise `sig` on every live member of a process group - what a ^C
 * does, and what SYS_kill's negative-pid form does. */
void sched_raise_signal_group(int pgid, int sig);

/* ---- M79 -------------------------------------------------------------
 *
 * The task that owns `t`'s address space bookkeeping - `t` itself for a
 * process, and the thread group's leader for a thread. Every field in
 * task_t that describes the *address space* rather than the *task* has
 * to be read and written through this: heap_brk, heap_mapped_end,
 * shm_next_vaddr and the mmap table. Two tasks sharing one page table
 * with two independent sbrk breaks would each grow into the other's
 * memory, which is a bug with no symptom until it has already happened.
 *
 * Returns `t` itself if the leader is gone, so a thread outliving its
 * leader degrades to "my own bookkeeping" rather than to a null
 * dereference. */
task_t *sched_vm_owner(task_t *t);

/* ---- M82: demand paging ----------------------------------------------
 *
 * The page-fault handler's one question: was this fault a program
 * touching a page of its own mmap for the first time?
 *
 * Returns 1 if so, having put a zeroed page there - the faulting
 * instruction is then restarted and sees memory. Returns 0 for
 * everything else, which leaves kernel/arch/x86_64/isr.c's existing
 * behaviour exactly as it was: a ring-3 fault kills the process, a
 * ring-0 fault panics. That split is the important part. A fault that
 * this fills is not a bug; every other fault still is, and this must
 * never turn one of those into a silent success.
 *
 * Lives here rather than in kernel/mm because the thing it consults is
 * the mmap table, and that is task state.
 */
/* M91: `user_rsp` is the faulting task's ring-3 stack pointer, needed
 * because the stack now grows on a fault and "is this a stack access"
 * cannot be answered from the address alone - see STACK_GROW_SLACK in
 * sched.c. */
/* M102: three outcomes, not two.
 *
 *   1               filled - the fault is answered, resume the program
 *   0               not this task's address - a real fault, SIGSEGV
 *   FILL_NO_MEMORY  it WAS this task's address and there is no memory
 *
 * The third used to be unreachable, because the allocator halted the
 * machine instead. Now that it is reachable it has to be told apart from
 * the second: "your pointer is wrong" and "the machine is full" are
 * different facts about different parties, and reporting the second as
 * the first would tell a program its own code is broken when it is not -
 * and would let it catch and ignore the news. */
#define FILL_NO_MEMORY (-1)
int sched_fault_fill(uint64_t addr, uint64_t error_code, uint64_t user_rsp);

/* M82: build every page of `addr`..`addr+len` that is a reserved-but-
 * untouched mmap page, so that a buffer a program obtained from mmap and
 * has not written to yet can still be passed to a syscall. See the
 * implementation for why this is done up front rather than by letting
 * the kernel's own copy loop fault. */
void sched_prefault_range(uint64_t addr, uint64_t len, int for_write);

/* M79: makes a new task in `pml4_phys` that is a *thread* of `leader` -
 * same address space, its own kernel stack, and its own entry. Used only
 * by kernel/proc/proc.c's process_spawn_thread; declared here because
 * only sched.c can set the fields that make it a thread. */
task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg);

/* M83: a task that resumes where its parent was, in `child_pml4`, with
 * `regs` as its user-visible register state and rax forced to 0. See the
 * implementation for what a fork inherits that a spawn does not, and why
 * each of those differs. Returns NULL if the task table is full. */
task_t *task_fork(uint64_t child_pml4, const isr_regs_t *regs);

/* M79: posts SIGKILL to every task in `t`'s thread group except `t`
 * itself. What SYS_exit does before ending the caller - POSIX exit()
 * ends a process, and a process is now more than one task. */
void sched_kill_thread_group(task_t *t);

/* M79: how many live tasks are running on `pml4_phys`. Exists for the
 * boot self-test, because "two of this scheduler's tasks share a page
 * table" is the entire claim of this milestone and it is not observable
 * from inside a program - from in there, a thread and a second process
 * with a very cooperative parent look identical. */
int sched_count_sharing_address_space(uint64_t pml4_phys);

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
/* M106: the high-water marks behind MAX_TASKS and MAX_FDS, so the day
 * either ceiling is approached the number says so rather than a spawn
 * failing. See sched.c. */
int sched_peak_live_tasks(void);
int sched_fd_high_water(int *which_task_out);

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
void sched_dump_cpus(void); /* M106 - see sched.c */

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

/* M59: drop / take one more reference to whatever a descriptor slot
 * points at. Every path that stops or starts holding a slot goes through
 * these, or the refcounts that now decide when a pipe closes and when an
 * open file is freed are simply wrong. */
void fd_release(fd_slot_t *slot);
void fd_retain(const fd_slot_t *slot);
/* Every one of a task's descriptors, released - what a task exiting owes
 * the rest of the system. */
void sched_release_fds(task_t *t);
