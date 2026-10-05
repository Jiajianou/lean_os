#pragma once

#include <stdint.h>

#include "capabilities.h"
#include "signal.h"
#include "paths.h"
#include "library/spinlock.h"
#include "memory_management/virtual_memory.h"
#include "architecture/x86_64/floating_point_unit.h"

#include "architecture/x86_64/interrupt_service_routines.h"
#include "process.h"

struct pipe;
struct memfd;
struct eventfd;
struct timerfd;
struct epoll;
struct unix_socket;

typedef enum {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_TERMINATED,
    TASK_BLOCKED,
    TASK_STOPPED,
} task_state_t;

typedef enum {
    FILE_DESCRIPTOR_NONE = 0,
    FILE_DESCRIPTOR_STDIN,
    FILE_DESCRIPTOR_STDOUT,
    FILE_DESCRIPTOR_PIPE_READ,
    FILE_DESCRIPTOR_PIPE_WRITE,
    FILE_DESCRIPTOR_FILE,
    FILE_DESCRIPTOR_SOCKET,
    FILE_DESCRIPTOR_UNIX,
    FILE_DESCRIPTOR_EVENT,
    FILE_DESCRIPTOR_TIMER,
    FILE_DESCRIPTOR_EPOLL,
    FILE_DESCRIPTOR_MEMFD,
    /* Claimed by a thread that has not finished filling it in. Numbering a
       descriptor and installing one are two steps, and a slot has to belong
       to somebody between them or another processor takes the same number. */
    FILE_DESCRIPTOR_RESERVED,
} file_descriptor_type_t;

/* Descriptors per process. 128 until M187, which was enough for every
   program this machine had run including content_shell - and not for
   Chromium's own browser, whose browser process holds its profile's
   databases, the sockets and pipes to each child, memfds, eventfds and an
   epoll set, and reached 128 fifteen seconds after starting. What it looked
   like from outside was M183's shape again: a forked child's
   open("/dev/null") failing so every child _exit(127)ed, and memfd_create
   failing so base fell back to /dev/shm and stopped on a FATAL about its
   permissions. 1024 is Linux's default soft limit, which is the number
   software written for Linux has been living inside; a slot is 16 bytes, so
   a table is 16 KB of kernel heap per process. */
#define MAX_FILE_DESCRIPTORS 1024

/* How many regions a process may describe. It is a ceiling rather than a
   size: the table below is allocated when a process first maps something and
   doubles from MMAP_REGIONS_INITIAL as it fills, so a process that maps
   nothing carries a pointer and a count rather than a table.

   It was a fixed array of 128 entries until M155 measured what a browser does
   to an address space. PartitionAlloc reserves one large range and then
   commits, decommits and protects sub-ranges of it, and every one of those
   splits an entry - so the count is not "how many things has this program
   mapped" but "how many different answers does one range have". V8 exhausts
   128 before it finishes starting.

   65536 is Linux's own neighbourhood (its default vm.max_map_count is 65530)
   and costs 2 MB for a process that actually reaches it. Nothing else here
   has ever needed more than a few dozen. */
#define MAX_MMAP_REGIONS      65536
#define MMAP_REGIONS_INITIAL  32

/* Every capacity the table can have, which is every doubling from
   MMAP_REGIONS_INITIAL up to MAX_MMAP_REGIONS. The last one is never retired,
   so this has one to spare. The assertion is what keeps it that way: move
   either bound and the build stops, rather than the machine. */
#define MMAP_RETIRED_MAX      12
_Static_assert(((uint64_t)MMAP_REGIONS_INITIAL << MMAP_RETIRED_MAX) >=
                   (uint64_t)MAX_MMAP_REGIONS,
               "a table that doubles from MMAP_REGIONS_INITIAL to "
               "MAX_MMAP_REGIONS leaves more retired tables than there is "
               "room to keep");

#define SCHEDULER_WATCH_MAX 64

typedef struct {
    uint64_t base;
    uint32_t pages;
    uint32_t prot;
    int handle;
    uint32_t file_page;
    uint8_t shared;
    uint32_t memfd_id;
    uint16_t memfd_gen;
} mmap_region_t;

#define TASK_NAME_MAX 24

#define KERNEL_ACTIVITY_NONE        0u
#define KERNEL_ACTIVITY_PAGE_FAULT  0xFFFFu

/* Tasks on the whole machine - and a thread is a task, so this is processes
   and threads together. 128 until M187, which Chromium's own browser crossed
   with its browser, renderer, network, storage and utility processes and the
   thread pools in each: a child's pthread_create failed and it stopped on a
   CHECK in base::SimpleThread. 256 is the most PID_SLOT_BITS can name, and
   the table comes from the page allocator rather than the image (M187: 1.1 MB
   more BSS was enough to stop the loader reserving the kernel's address).
   512 since M205: on the laptop's eight threads Chromium sizes its pools to
   the processor count, and a renderer a site is fifteen or twenty threads -
   one browser with a handful of sites open came within a few slots of 256
   once the slots of dead threads came back at all. Three megabytes. */
#define MAX_TASKS 512

#define PID_SLOT_BITS 9
_Static_assert(MAX_TASKS <= (1 << PID_SLOT_BITS), "a pid names its slot in PID_SLOT_BITS bits");
_Static_assert(TASK_INFO_MAX >= MAX_TASKS, "SYS_taskinfo must be able to name every task there can be");
#define PID_SLOT_MASK ((1 << PID_SLOT_BITS) - 1)
#define PID_MAKE(slot, gen) (((gen) << PID_SLOT_BITS) | (slot))
#define PID_SLOT(pid) ((pid) & PID_SLOT_MASK)
#define PID_GEN(pid) (((unsigned)(pid)) >> PID_SLOT_BITS)

typedef struct {
    file_descriptor_type_t type;
    union {
        struct pipe *pipe;
        struct open_file *file;
        struct socket *sock;
        struct unix_socket *un;
        struct eventfd *event;
        struct timerfd *timer;
        struct epoll *epoll;
        struct memfd *memfd;
    };
    uint8_t cloexec;
    uint8_t nonblock;
    /* The access this DESCRIPTOR has, as opposed to the access the object
       allows. A memfd is created writable and a second descriptor for the
       same pages, reopened through /proc/<pid>/fd/<n>, can be read-only -
       which is what lets a process hand out memory somebody else can read
       and cannot change. It only ever shrinks, like the capability set.
       Types whose access is fixed by what they are - a pipe end, an epoll
       set - do not consult it. */
    uint8_t writable;
} file_descriptor_slot_t;

/* One table per process, reference counted: a thread shares its creator's
   and a fork gets a copy. The count is the number of TASKS pointing here,
   which for a single-threaded process is one. */
typedef struct file_descriptor_table {
    file_descriptor_slot_t slots[MAX_FILE_DESCRIPTORS];
    int references;
    /* M225: who may take a descriptor out of a slot, or copy one with a
       reference of its own. A leaf: what is done under it is reading and
       writing slots and taking an object's reference (each object's own lock,
       never anything that sleeps), and it is never taken under
       scheduler_lock. Releasing an object is never done under it - a slot is
       detached here and the object let go of after. */
    spinlock_t lock;
} file_descriptor_table_t;

/* M225 (fd-use-holds): one system call's use of one descriptor - Linux's
   fdget/fdput. `slot` is a copy of what the descriptor named when the call
   began; `counted` says the copy holds a reference of its own, which
   file_descriptor_put gives back. A counted use is on its task's list
   (task_t.descriptor_uses) while the call runs, because a task here can end
   INSIDE a call - a fatal signal is acted on in the middle of a blocking
   wait, or from the timer tick - and the exit gives back whatever its calls
   still held. The records live on the kernel stack of the call that made
   them, which is the stack the exit runs on. */
typedef struct file_descriptor_use {
    file_descriptor_slot_t slot;
    struct file_descriptor_use *next;
    uint8_t counted;
} file_descriptor_use_t;

typedef enum {
    PRIO_INTERACTIVE = 0,
    PRIO_BATCH = 1,
} prio_class_t;

#define SCHEDULER_BATCH_THRESHOLD 10
#define SCHEDULER_AGING_TICKS     100

typedef struct task {
    uint64_t rsp;
    uint8_t *stack_base;
    uint64_t kernel_stack_top;
    task_state_t state;
    int id;
    unsigned generation;
    void (*entry)(void *arg);
    void *arg;
    uint64_t pml4_phys;
    int exit_code;
    int exit_signal;
    int pending_stop;
    int stopped_sig;
    uint8_t stop_reported;
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));
    /* The descriptor table belongs to the PROCESS. Every thread of one
       points at the same table, so a descriptor opened on any of them is
       open on all of them - which is what POSIX says and what a program
       that opens a file on one thread and reads it on another needs.
       Before M146 each thread got a COPY of its creator's table with every
       close-on-exec slot cleared, which made close-on-exec mean close on
       thread creation as well. */
    file_descriptor_table_t *descriptor_table;
    /* M225 (fd-use-holds): the counted descriptor uses this task's current
       system call holds, newest first - see file_descriptor_use_t. Only the
       task itself reads or writes it. */
    file_descriptor_use_t *descriptor_uses;
    int parent_id;
    /* M224: who this task descends from, for kill(2)'s permission check
       only. It starts as parent_id, and when a process's slot goes its
       children's lineage moves to that process's own lineage - so an orphan
       stays a descendant of whoever started the process that orphaned it.
       parent_id cannot do this: it is also who may WAIT, and a grandparent
       that never asked for these children must not find them in waitpid(-1).
       Without it the walk stopped at the first reaped process, and a test
       runner could not kill a worker its own test had left spinning - the
       one that then starved every test after it. */
    int lineage_id;
    uint32_t caps;
    int pgid;
    int sid;
    uint64_t fs_base;
    int pending_signal;
    uint64_t alarm_deadline_ms;
    int reaped;
    uint64_t heap_brk;
    uint64_t heap_mapped_end;
    uint64_t shared_memory_next_vaddr;
    const void *wait_chan;
    uint64_t wait_space;
    /* M197: the kernel objects a poller is waiting on - see
       scheduler_watch_begin. */
    const void *watch_objects[SCHEDULER_WATCH_MAX];
    uint32_t watch_count;
    uint8_t watching;
    uint8_t watch_everything;
    uint8_t watch_fired;
    /* M198: sleep locks this task holds. A signal is not delivered while it
       is above zero - see sleep_lock_acquire. */
    uint32_t sleep_locks_held;
    /* M199: the sleep lock this task is waiting for and since when, so a
       wait that never ends can be named along with the lock's holder. */
    const void *sleep_lock_waiting;
    uint64_t sleep_lock_wait_since_ms;
    uint8_t lock_wait_reported;
    /* M199: user and kernel ticks as the stick log's last line saw them,
       so the next line can say who used the processor in between. */
    uint64_t stamp_user_seen;
    uint64_t stamp_sys_seen;
    /* M199: when this task last got a processor, for the detector's
       "ready and never picked" check. */
    uint64_t last_ran_ms;
    uint8_t starvation_reported;
    /* M202: what the kernel is doing on this task's behalf - a system call
       number plus one, KERNEL_ACTIVITY_PAGE_FAULT, or zero - so a tick that
       lands in the kernel can say which request the time belongs to. */
    uint16_t kernel_activity;
    uint64_t ready_since_ms;
    /* M203: system calls this task has made, and as the stick log's last
       line saw them - "who is calling" is not "who is using the processor",
       and the laptop's worst storm was a process at 90% of a core that was
       mostly being woken. */
    uint64_t syscalls;
    uint64_t stamp_syscalls_seen;
    uint64_t woken_at_ns;
    uint8_t woken_by_timer;
    uint64_t seen_exit_sequence;
    uint64_t wake_deadline_ms;
    uint8_t prio;
    uint8_t full_slices;
    uint64_t last_block_tick;
    uint8_t is_idle;
    /* M225: the kernel itself - the boot task, an idle identity, or a thread
       that runs in the kernel's own address space (journal, tcp-timer, the
       drivers' pollers). Set when the slot is filled and never changed: a
       user process runs in the kernel's address space for the last moments
       of its exit, so "which page table is loaded" cannot answer this. No
       signal a user process sends reaches one of these - see scheduler_kill. */
    uint8_t kernel_task;
    /* M225: on a process's first task, the most of its tasks that have been
       alive at once - see scheduler_thread_group_peak. */
    uint32_t group_peak;
    /* M225: what this task's most recent spawn made - its id, recorded under
       the scheduler lock as the slot is filled, and its capabilities once
       the spawner has narrowed them. Read back by this task alone, through
       scheduler_last_spawn: the task_t a spawn returns is only the child's
       for as long as the child lives, and a child of the kernel that ends at
       once is swept by the next spawn on any processor. */
    int spawned_id;
    uint32_t spawned_caps;
    /* M175: -1 for a task that allocated a kernel stack of its own and can
       therefore run anywhere. A task that adopted the stack a CPU was already
       standing on - the boot task, and each AP's idle identity - records that
       CPU here, because its stack has exactly one owner and running it
       elsewhere puts two cores on one stack. */
    int home_cpu;
    uint64_t user_ticks;
    uint64_t sys_ticks;
    uint64_t child_user_ticks;
    uint64_t child_sys_ticks;
    uint64_t dead_thread_user_ticks;
    uint64_t dead_thread_sys_ticks;
    uint64_t max_rss_pages;
    uint64_t child_max_rss_pages;
    uint8_t idle_wait_depth;
    char cwd[PATH_MAX_LENGTH];
    char *env_block;
    /* The argument vector this program was started with, NUL-separated, as
       /proc/<pid>/cmdline(5) is defined to hold it. It belongs to the
       ADDRESS SPACE rather than to the task - every thread of a process has
       the same command line - so it is written by a spawn or an exec and
       released by the reap of the last member of the group, which is where
       M168 put everything else of that kind. */
    char *cmdline_block;
    uint32_t cmdline_length;
    uint32_t env_length;
    uint32_t env_count;
    uint64_t sig_handler[SIG_MAX + 1];
    uint64_t sig_restorer;
    uint32_t sig_pending;
    uint32_t sig_blocked;
    uint32_t sig_siginfo;
    uint32_t sig_onstack;
    /* M224: SA_RESETHAND, per signal - the handler is put back to SIG_DFL as
       it is entered. Node's SIGTERM handler restores the terminal and
       re-raises; without this the re-raise ran the handler again, for ever,
       and a process that had been told to terminate never did. */
    uint32_t sig_resethand;
    uint64_t sig_alt_stack_base;
    uint64_t sig_alt_stack_size;
    uint8_t  sig_on_alt_stack;
    int32_t  si_pid;
    int32_t  si_status;
    uint64_t si_address;
    int32_t  si_fault_code;
    /* Allocated on the first mapping and grown as it fills - see
       MAX_MMAP_REGIONS above. Zero and NULL until then, which every loop over
       it tolerates because they are all bounded by mmap_capacity. */
    mmap_region_t *mmaps;
    uint32_t mmap_capacity;
    /* M179: the tables this one grew out of. A page fault reads the region
       table without any lock - it cannot take one, because a fault is how the
       kernel finds out it needs memory in the first place - so a table that
       another thread has replaced may still be being walked when the
       replacement is published. Freeing it there is a use-after-free that the
       heap turns into somebody else's data. They are kept instead and go back
       together when the address space does; the capacity doubles from
       MMAP_REGIONS_INITIAL to MAX_MMAP_REGIONS, so there is a fixed and small
       number of them and their total is smaller than the live one. */
    mmap_region_t *mmaps_retired[MMAP_RETIRED_MAX];
    uint32_t mmaps_retired_count;
    /* M181: the region table belongs to the ADDRESS SPACE - every thread
       reaches it through scheduler_vm_owner - so the lock lives on the owner
       and all of them contend for the one. M179 made the table safe to READ
       while it grows and M180 made the fault path read it in one place; this
       is what makes it safe to CHANGE while somebody reads.

       Two rules. Nothing held across it may FAULT, because the page fault
       handler takes it and a writer that faulted while holding it would wait
       for itself - so no user memory inside, which is why sys_mincore copies
       its answer out after letting go. kmalloc IS allowed: the heap maps its
       pages rather than faulting them in, and scheduler_init_ap already
       allocates under scheduler_lock for the same reason.

       And the order against virtual_memory_lock is this one first, on both
       sides: a writer unmapping takes it and then reaches
       virtual_memory_unmap_range_free, and the fault path looks a region up
       and then calls virtual_memory_try_map_page_in. Same order is what makes
       it not an inversion. */
    spinlock_t mmap_lock;
    /* M225: odd while a writer is part way through changing the table, and
       one higher every time it starts or finishes - the "seqlock" M181 named
       as the way to close the last hole in a reader that takes no lock.
       Writers bump it under mmap_lock (scheduler_regions_begin_change); the
       fault path reads it before and after its unlocked look and, if a writer
       was or had been in the table meanwhile, looks again under the lock.
       Only that rare look waits, so the M181 measurement still stands. */
    uint32_t mmap_sequence;
    int tgid;
    uint8_t is_thread;
    /* M205: nobody will join this thread - pthread_detach said so - so once
       it has terminated its slot is anybody's. */
    uint8_t detached;
    /* M206: somebody is reaping this slot right now. Since M205 a slot has
       three reapers - wait(), the sweep a spawn runs, and the reap of a
       group's last thread - and on a machine that preempts them the first
       could free the kernel stack while a second was still deciding to. */
    uint8_t reaping;
    uint8_t exiting;
    /* M225: this task has begun to exit and takes no more signals. Set under
       the scheduler lock at the top of task_exit_with_code, which is also
       where raise_signal_locked looks. Before it, a sibling dying of the
       SIGKILL that this task's own exit had sent it sent one straight back
       (scheduler_kill_thread_group from its exit) while this task was still
       tearing itself down, and the next timer tick delivered it: the exit
       began again from the top, and the process's status became SIGKILL
       whatever had really ended it - forktest's "exited 17" on four
       processors, a SIGTERM that waitpid reported as signal 9. */
    uint8_t ending;
    char name[TASK_NAME_MAX];
    /* M223. The program this task is running, which is not its name: a
       name is what pthread_setname_np last said - Node calls its main
       thread "MainThread" - and /proc/<pid>/exe answered with it, so
       process.execPath became /bin/MainThread. Set when a process is spawned
       or execs, inherited by its threads, and never renamed. */
    char program[TASK_NAME_MAX];
} task_t;

void scheduler_init(void);

void scheduler_init_ap(int cpu_id);

void scheduler_tick_cpu(int cpu);

void scheduler_block_on(const void *chan, uint64_t deadline_ms, spinlock_t *lock, uint64_t *flags);
void scheduler_block_on_in_space(const void *chan, uint64_t space, uint64_t deadline_ms,
                                 spinlock_t *lock, uint64_t *flags);

/* A sleep that gives the CPU up rather than halting on it. See the comment
   at the definition: pit_sleep_ms() stays runnable, which costs a share of a
   busy machine for every task that is asleep in it. */
void scheduler_sleep_ms(uint32_t ms);

void scheduler_halt(void);

/* M197: a lock a task sleeps on rather than spins for, held with interrupts
   ON so its holder can be preempted and its waiters cost nothing. */
typedef struct {
    spinlock_t guard;
    volatile int held;
    int waiters;
    int holder;
} sleep_lock_t;

void sleep_lock_acquire(sleep_lock_t *lock);
void sleep_lock_release(sleep_lock_t *lock);

void scheduler_watch_begin(void);
void scheduler_watch_add(const void *object);
void scheduler_watch_block(uint64_t deadline_ms);
void scheduler_watch_end(void);
void scheduler_wake_object(const void *object);
void scheduler_wake_objects(const void *first, const void *second);
uint64_t scheduler_exit_sequence(void);
uint64_t scheduler_halted_us(int cpu);

void scheduler_wake_all(const void *chan);

int scheduler_wake_n(const void *chan, uint64_t space, int max);

uint64_t scheduler_event_sequence(void);

void scheduler_block_on_sequence(const void *chan, uint64_t deadline_ms, uint64_t expected_sequence);

extern const int scheduler_poll_channel;
#define SCHEDULER_POLL_CHAN (&scheduler_poll_channel)

extern const int scheduler_keyboard_channel;
/* M200: what a key or a mouse movement wakes. It used to wake every poller
   on the machine - every Chromium thread in epoll_wait, every desktop
   program - a hundred and twenty-five times a second while a finger moved
   on the touchpad, each to find nothing for it and sleep again. Only a wait
   that includes the console's input descriptor watches this now; a poller
   that watches nothing in particular is still woken by it. */
extern const int scheduler_input_object;
#define SCHEDULER_INPUT_OBJECT ((const void *)&scheduler_input_object)
#define SCHEDULER_SLEEP_CHAN (&scheduler_sleep_channel)
#define SCHEDULER_KEYBOARD_CHAN (&scheduler_keyboard_channel)

void scheduler_spawn_idle_tasks(void);

void scheduler_mark_self_idle(void);


void scheduler_idle_enter(void);
void scheduler_idle_exit(void);

int scheduler_task_is_idle_waiting(const task_t *t);

void scheduler_debug_dump(const char *label);

uint64_t scheduler_idle_ticks(int cpu);
uint64_t scheduler_total_ticks(int cpu);

void scheduler_account_tick(int user);

task_t *task_spawn(const char *name, void (*entry)(void *arg), void *arg);

int scheduler_has_free_task_slot(void);

task_t *task_spawn_in(const char *name, uint64_t pml4_phys, void (*entry)(void *arg), void *arg,
                       uint64_t heap_start, uint64_t shared_memory_base);

/* M225: what a new process is given before it can run. task_spawn_program
   writes all of it into the slot under the lock the slot is filled under,
   before the task is READY - so nothing about the child is written after
   another processor could have run it, ended it and handed its slot on.
   The two blocks are kmalloc'd by the caller and owned by the new task once
   the spawn succeeds; on failure they are still the caller's to free. */
typedef struct {
    uint32_t caps;
    char *cmdline;
    uint32_t cmdline_length;
    char *env;
    uint32_t env_length;
    uint32_t env_count;
} task_spawn_setup_t;

task_t *task_spawn_program(const char *name, uint64_t pml4_phys, void (*entry)(void *arg),
                           void *arg, uint64_t heap_start, uint64_t shared_memory_base,
                           const task_spawn_setup_t *setup);

void schedule(void);


void task_exit(void) __attribute__((noreturn));
void task_exit_with_code(int code) __attribute__((noreturn));

task_t *scheduler_current(void);

task_t *scheduler_task_by_id(int pid);

int scheduler_set_env(task_t *t, const char *block, uint32_t length, uint32_t count);

void scheduler_set_task_name(task_t *t, const char *name);

/* A thread's display name alone - pthread_setname_np - leaving the program
   it runs as it was. */
void scheduler_set_thread_name(task_t *t, const char *name);

void task_exit_with_signal(int sig) __attribute__((noreturn));

int scheduler_wait_status(const task_t *t);
int scheduler_recent_exit_status(int pid, int *status_out);
/* M225: how the process `pid` ended, as wait() would say it, from its slot
   while that is a zombie and from the record of recent exits once the slot
   has been released. 1 and the wait status (scheduler_wait_status's
   encoding) if it has ended; 0 if it is still running; -1 if nothing knows
   the id - never valid, or ended so long ago the record has moved on. Read
   under the scheduler lock with the generation checked, so a slot released
   and refilled while it is being read is never mistaken for the process. */
int scheduler_exit_status(int pid, int *status_out);
/* M225: the same answer as an exit code: WEXITSTATUS of an exit, 128 plus
   the signal for a death by signal - so an exit(256) reads 0 from the slot
   and from the record alike, which is what waitpid reports. Same 1/0/-1. */
int scheduler_exit_code(int pid, int *code_out);
/* M225: what wait()'s status means as an exit code, the one decoding both
   of the above and the boot self-tests use. */
int scheduler_exit_code_of_status(int status);

/* M225: the kernel - the boot task, an idle task, a kernel thread. */
int scheduler_task_is_kernel(const task_t *t);

/* M225: kill(2) itself: who `pid` names and what reaches them, with
   `may_signal` deciding permission for each target that is not ruled out
   here. pid > 0 is that task; 0 is the caller's process group; -1 is every
   process the caller may signal but its own (POSIX, as Linux and the BSDs
   read it); below -1 is the process group -pid. A kernel task is never a
   target unless the caller is the kernel, and process group 0 - the kernel's,
   which is not a process group - is nobody's. 0, -OS_ERROR_SEARCH,
   -OS_ERROR_PERMISSION or -OS_ERROR_INVALID. */
long scheduler_kill(task_t *self, long pid, int sig,
                    int (*may_signal)(task_t *self, task_t *target));

/* M225: the task a pid ARGUMENT names, where 0 means the caller - getpgid(0)
   and getsid(0). scheduler_task_by_id(0) is the kernel task. */
task_t *scheduler_task_for_pid_argument(int pid);
/* M225: the most tasks of process `pid` alive at once, or -1. */
int scheduler_thread_group_peak(int pid);

/* M225: what every wait() asks - has t's PROCESS ended: t has terminated and,
   for a process leader, so has every one of its threads. */
int scheduler_process_has_ended(const task_t *t);

/* M225: the id of the task the CALLING task last spawned (0 if none) and, in
   *caps_out when it is not null, the capabilities it was given. */
int scheduler_last_spawn(uint32_t *caps_out);

/* M225: the orderly stop's signal to every task it covers - each one but
   `self` that has a parent and has not ended - through the same door as
   kill(2), so a handler runs and a sleeper wakes. Returns how many it
   reached. */
int scheduler_signal_orderly_stop(task_t *self, int sig);

/* setpgid(2) for `self`: 0, or -1 if refused. */
int scheduler_set_process_group(task_t *self, int pid, int pgid);

/* M225: whether SYS_wait from `self` may wait for (and reap) `t`. */
int scheduler_may_wait_for(const task_t *self, const task_t *t);

void scheduler_release_env(task_t *t);

/* TASK_CMDLINE_MAX is a ceiling rather than a size: what it has to be bigger
   than is a real command line, and Chromium's renderer is started with about
   twenty arguments. Linux's own limit for the same file is a page. */
#define TASK_CMDLINE_MAX 4096
void scheduler_set_cmdline(task_t *t, const char *const *argv);
/* argv packed the way /proc/<pid>/cmdline reads it, in a block of its own
   (kmalloc'd; *length_out bytes), or 0 when there is nothing to pack. */
char *scheduler_pack_cmdline(const char *const *argv, uint32_t *length_out);
void scheduler_release_cmdline(task_t *t);
/* M225 (process-lifetimes): /proc/<pid>/cmdline's bytes for task `pid` - its
   process's argument vector, or its name and a NUL when none was recorded -
   COPIED into `out` (at most `capacity` bytes) under the scheduler lock, so
   a reap or an exec freeing the record cannot free it under the reader.
   Returns the bytes written, or -1 when `pid` names no task. */
int scheduler_copy_cmdline(int pid, char *out, uint32_t capacity);
/* The same for t's process's environment record (NUL-separated, *count_out
   strings): the bytes written, or 0 when there is none or it is larger than
   `capacity`. */
uint32_t scheduler_copy_env(task_t *t, char *out, uint32_t capacity, uint32_t *count_out);
/* M225 (fd-use-holds): the same record in a kmalloc'd block of exactly its
   size, or null when there is none (*no_memory says when one could not be
   made) - what exec inherits when it is handed no environment. */
char *scheduler_duplicate_env(task_t *t, uint32_t *length_out, uint32_t *count_out,
                              int *no_memory);

/* M225 (process-lifetimes): who owns t's fcntl record locks - its PROCESS,
   the id getpid() answers - for every flock_* call to key on. */
int scheduler_record_lock_owner(const task_t *t);

unsigned int scheduler_set_alarm(task_t *t, unsigned int seconds);

int scheduler_release_shared_range(task_t *t, uint64_t start, uint64_t end);

/* Makes pml4_phys this task's address space and loads it on this core,
   recording the load where TLB shootdowns look for it. */
void scheduler_load_address_space(task_t *t, uint64_t pml4_phys);

/* The ranges of t's memfd and shared file mappings, sorted by address, in a
   kmalloc'd array the caller frees; the count, or -1 if there was no memory
   for the array. What fork leaves shared rather than copy-on-write. */
int scheduler_shared_ranges(task_t *t, virtual_memory_range_t **out);

void scheduler_region_forget_memfd(mmap_region_t *r);
/* Make room for one more region, growing the table if it is full. Returns 0
   when t->mmap_capacity is greater than the number of regions in use - so
   there is a free entry AND a zero-`pages` one after it, which is the
   terminator every reader stops at - and -1 when the ceiling is reached or
   the heap refuses. Both of these take the heap lock, so nothing holding it
   may call them; the scheduler lock is the other way round and that ordering
   is one-way, because the heap never reaches the scheduler. */
int scheduler_regions_reserve(task_t *t);

/* M181: taken around every look at the table that has to see it whole.
   Callers outside the scheduler go through these so that what the lock
   protects stays one thing. See task_t::mmap_lock for the two rules. */
uint64_t scheduler_regions_lock(task_t *t);
void scheduler_regions_unlock(task_t *t, uint64_t flags);

/* M225: the same lock, taken by something that CHANGES the table - inserts,
   removes, merges, splits or edits a region. It also marks the table as
   changing (task_t::mmap_sequence), which is what tells a page fault that
   its unlocked look may have read an entry half way through moving and must
   be asked again. A writer that took scheduler_regions_lock instead would
   leave that hole open. */
uint64_t scheduler_regions_begin_change(task_t *t);
void scheduler_regions_end_change(task_t *t, uint64_t flags);

/* Does any region cover this page? May take the lock itself (when a writer is
   or was in the table during its look), so the caller must not hold it. */
int scheduler_region_covers(task_t *t, uint64_t page);

/* Give the table back. Safe on a task that never mapped anything. */
void scheduler_regions_release(task_t *t);

void scheduler_regions_forget_memfds(task_t *t);
void scheduler_regions_retain_memfds(task_t *t);

void scheduler_raise_signal(task_t *t, int sig);
/* M225: the calling task is on its way out - no signal reaches it from here
   (task_t::ending). The first thing every exit does, before it signals the
   rest of its process. */
void scheduler_begin_exit(task_t *t);
/* M225: raise `sig` on t only if, under the scheduler lock, t is still alive,
   still `expected_id` (0: whoever is there - for a caller that holds the task
   itself) and not a kernel task unless `from_kernel`. sig 0 only asks.
   Returns whether t was that task. */
int scheduler_raise_signal_checked(task_t *t, int expected_id, int sig, int from_kernel);

int scheduler_signal_pending(void);

void scheduler_wake_task(task_t *t);

void scheduler_resume_stopped(task_t *t);

void scheduler_take_pending_stop_if_any(task_t *t);

void scheduler_raise_signal_group(int pgid, int sig);

task_t *scheduler_vm_owner(task_t *t);

#define FILL_NO_MEMORY (-1)
int scheduler_fault_fill(uint64_t address, uint64_t error_code, uint64_t user_rsp);
int scheduler_address_is_mapped(uint64_t address);

void scheduler_prefault_range(uint64_t address, uint64_t length, int for_write);

/* A thread of `leader`'s process. Only a task running IN that process may ask
   (the leader itself or one of its threads), and anything else is refused
   with 0: the new thread's reference on the descriptor table is only safe to
   take while the caller holds one of its own. */
task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg);

task_t *task_fork(uint64_t child_pml4, const isr_regs_t *regs);

void scheduler_kill_thread_group(task_t *t);

void scheduler_thread_group_ticks(task_t *t, uint64_t *user_ticks_out, uint64_t *sys_ticks_out);

int scheduler_count_sharing_address_space(uint64_t pml4_phys);

task_t *scheduler_task_by_slot(int slot);
void scheduler_forget_address_space(uint64_t pml4_phys);

/* The cores whose CR3 currently holds this address space (bit n is cpu n) -
   the ones that can have its translations cached, and so the only ones a
   change to its page tables has to reach. */
uint32_t scheduler_cpus_holding_address_space(uint64_t pml4_phys);

/* The reschedule IPI's handler: an idle core that was asked looks for work. */
void scheduler_reschedule_if_idle(void);
int scheduler_address_space_is_shared(task_t *owner);

int scheduler_task_count(void);

int scheduler_live_task_count(void);
int scheduler_peak_live_tasks(void);
int scheduler_file_descriptor_high_water(int *which_task_out);

void scheduler_reap_slot(task_t *t);

int scheduler_detach_thread(task_t *caller, int thread_id);

void scheduler_release_finished_tasks(void);
void scheduler_dump_cpus(void);

void scheduler_deadline_timer_fired(void);
uint64_t scheduler_switch_count(int cpu);
uint64_t scheduler_deadline_timer_interrupts(void);
uint64_t scheduler_deadline_timer_lost(void);
int scheduler_task_slot_count(void);
task_t *scheduler_task_slot(int index);
task_t *scheduler_cpu_current(int cpu);
void scheduler_dump_stack_owner(uint64_t address);

void scheduler_reset_file_descriptors_to_std(task_t *t);

file_descriptor_table_t *descriptor_table_new(void);
void descriptor_table_reference(file_descriptor_table_t *table);
void descriptor_table_release(file_descriptor_table_t *table);

void file_descriptor_release(file_descriptor_slot_t *slot);
void file_descriptor_retain(const file_descriptor_slot_t *slot);

/* M225: the slot protocol. A table is shared by every thread of a process,
   so a slot can be closed on one processor while another reads it. Every
   operation that takes a descriptor OUT of a slot or copies one with a
   reference of its own does it under the table's lock, and the object is
   let go of after the lock is: see scheduler.c. "Live" is any type but NONE
   and RESERVED - a reserved slot belongs to the thread filling it in and is
   nobody else's to close, copy or replace. */
#define DESCRIPTOR_TABLE_BAD  (-1) /* the descriptor names nothing live */
#define DESCRIPTOR_TABLE_BUSY (-2) /* the target is being filled in by another thread */
#define DESCRIPTOR_TABLE_FULL (-3) /* no free slot at or above the one asked for */

int file_descriptor_claim(file_descriptor_table_t *table, int from);
void file_descriptor_unclaim(file_descriptor_table_t *table, int fd);
void file_descriptor_install(file_descriptor_table_t *table, int fd,
                             const file_descriptor_slot_t *slot);
int file_descriptor_hold(file_descriptor_table_t *table, int fd, file_descriptor_slot_t *out);
/* M225 (fd-use-holds): what every system call that USES a descriptor does
   first and last - see scheduler.c. 0 or DESCRIPTOR_TABLE_BAD (nothing live
   there, or no table). Every successful get is followed by exactly one put,
   on the same task; file_descriptor_put_all is the exit's. */
int file_descriptor_get_shared(task_t *t, int fd, file_descriptor_use_t *use);
static inline int file_descriptor_get(task_t *t, int fd, file_descriptor_use_t *use) {
    file_descriptor_table_t *table = t ? t->descriptor_table : (file_descriptor_table_t *)0;
    if (table && fd >= 0 && fd < MAX_FILE_DESCRIPTORS &&
        __atomic_load_n(&table->references, __ATOMIC_ACQUIRE) == 1) {
        /* A table only this task names: see file_descriptor_get_shared. */
        file_descriptor_type_t type = __atomic_load_n(&table->slots[fd].type, __ATOMIC_ACQUIRE);
        use->counted = 0;
        if (type == FILE_DESCRIPTOR_NONE || type == FILE_DESCRIPTOR_RESERVED) {
            return DESCRIPTOR_TABLE_BAD;
        }
        use->slot = table->slots[fd];
        return 0;
    }
    return file_descriptor_get_shared(t, fd, use);
}
void file_descriptor_put(task_t *t, file_descriptor_use_t *use);
void file_descriptor_put_all(task_t *t);
/* Panics if `t` has a use not yet put - run as a system call returns. */
void file_descriptor_uses_settled(task_t *t);
file_descriptor_type_t file_descriptor_peek_type(task_t *t, int fd);
int file_descriptor_detach(file_descriptor_table_t *table, int fd, file_descriptor_slot_t *out);
int descriptor_table_duplicate(file_descriptor_table_t *table, int oldfd, int newfd,
                               int cloexec, file_descriptor_slot_t *displaced);
int descriptor_table_duplicate_lowest(file_descriptor_table_t *table, int oldfd, int from,
                                      int cloexec);
void descriptor_table_copy(file_descriptor_table_t *to, file_descriptor_table_t *from,
                           int for_exec, int keep_below);
int file_descriptor_set_flags(file_descriptor_table_t *table, int fd, int cloexec,
                              int nonblock);
int descriptor_table_detach_cloexec(file_descriptor_table_t *table, int from,
                                    file_descriptor_slot_t *out);
void scheduler_release_file_descriptors(task_t *t);
/* How many descriptors a task has open, read under the scheduler lock so the
   table cannot be freed under the count (0 for a task that has none). */
int scheduler_open_descriptor_count(task_t *t);
