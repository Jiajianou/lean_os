#pragma once

#include <stdint.h>

#include "capabilities.h"
#include "signal.h"
#include "paths.h"
#include "library/spinlock.h"
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
} file_descriptor_type_t;

#define MAX_FILE_DESCRIPTORS 128

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

#define MAX_TASKS 128

#define PID_SLOT_BITS 8
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
} file_descriptor_table_t;

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
    int parent_id;
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
    uint64_t wake_deadline_ms;
    uint8_t prio;
    uint8_t full_slices;
    uint64_t last_block_tick;
    uint8_t is_idle;
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
    int tgid;
    uint8_t is_thread;
    uint8_t exiting;
    char name[TASK_NAME_MAX];
} task_t;

void scheduler_init(void);

void scheduler_init_ap(int cpu_id);

void scheduler_tick_cpu(int cpu);

void scheduler_block_on(const void *chan, uint64_t deadline_ms, spinlock_t *lock, uint64_t *flags);

/* A sleep that gives the CPU up rather than halting on it. See the comment
   at the definition: pit_sleep_ms() stays runnable, which costs a share of a
   busy machine for every task that is asleep in it. */
void scheduler_sleep_ms(uint32_t ms);

void scheduler_wake_all(const void *chan);

int scheduler_wake_n(const void *chan, int max);

uint64_t scheduler_event_sequence(void);

void scheduler_block_on_sequence(const void *chan, uint64_t deadline_ms, uint64_t expected_sequence);

extern const int scheduler_poll_channel;
#define SCHEDULER_POLL_CHAN (&scheduler_poll_channel)

extern const int scheduler_keyboard_channel;
#define SCHEDULER_SLEEP_CHAN (&scheduler_sleep_channel)
#define SCHEDULER_KEYBOARD_CHAN (&scheduler_keyboard_channel)

void scheduler_spawn_idle_tasks(int cpus);

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

void schedule(void);


void task_exit(void) __attribute__((noreturn));
void task_exit_with_code(int code) __attribute__((noreturn));

task_t *scheduler_current(void);

task_t *scheduler_task_by_id(int pid);

int scheduler_set_env(task_t *t, const char *block, uint32_t length, uint32_t count);

void scheduler_set_task_name(task_t *t, const char *name);

void task_exit_with_signal(int sig) __attribute__((noreturn));

void scheduler_release_env(task_t *t);

/* TASK_CMDLINE_MAX is a ceiling rather than a size: what it has to be bigger
   than is a real command line, and Chromium's renderer is started with about
   twenty arguments. Linux's own limit for the same file is a page. */
#define TASK_CMDLINE_MAX 4096
void scheduler_set_cmdline(task_t *t, const char *const *argv);
void scheduler_release_cmdline(task_t *t);
const char *scheduler_cmdline(task_t *t, uint32_t *length);

unsigned int scheduler_set_alarm(task_t *t, unsigned int seconds);

int scheduler_release_shared_range(task_t *t, uint64_t start, uint64_t end);

void scheduler_region_forget_memfd(mmap_region_t *r);
/* Make room for one more region, growing the table if it is full. Returns 0
   when t->mmap_capacity is greater than the number of regions in use - so
   there is a free entry AND a zero-`pages` one after it, which is the
   terminator every reader stops at - and -1 when the ceiling is reached or
   the heap refuses. Both of these take the heap lock, so nothing holding it
   may call them; the scheduler lock is the other way round and that ordering
   is one-way, because the heap never reaches the scheduler. */
int scheduler_regions_reserve(task_t *t);

/* Give the table back. Safe on a task that never mapped anything. */
void scheduler_regions_release(task_t *t);

void scheduler_regions_forget_memfds(task_t *t);
void scheduler_regions_retain_memfds(task_t *t);

void scheduler_raise_signal(task_t *t, int sig);

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

task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg);

task_t *task_fork(uint64_t child_pml4, const isr_regs_t *regs);

void scheduler_kill_thread_group(task_t *t);

void scheduler_thread_group_ticks(task_t *t, uint64_t *user_ticks_out, uint64_t *sys_ticks_out);

int scheduler_count_sharing_address_space(uint64_t pml4_phys);

task_t *scheduler_task_by_slot(int slot);
void scheduler_forget_address_space(uint64_t pml4_phys);
int scheduler_address_space_is_shared(task_t *owner);

int scheduler_task_count(void);

int scheduler_live_task_count(void);
int scheduler_peak_live_tasks(void);
int scheduler_file_descriptor_high_water(int *which_task_out);

void scheduler_reap_slot(task_t *t);
void scheduler_dump_cpus(void);

void scheduler_reset_file_descriptors_to_std(task_t *t);

file_descriptor_table_t *descriptor_table_new(void);
void descriptor_table_reference(file_descriptor_table_t *table);
void descriptor_table_release(file_descriptor_table_t *table);

void file_descriptor_release(file_descriptor_slot_t *slot);
void file_descriptor_retain(const file_descriptor_slot_t *slot);
void scheduler_release_file_descriptors(task_t *t);
