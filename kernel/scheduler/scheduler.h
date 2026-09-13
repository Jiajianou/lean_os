#pragma once

#include <stdint.h>

#include "caps.h"
#include "signal.h"
#include "paths.h"
#include "library/spinlock.h"
#include "architecture/x86_64/floating_point_unit.h"

#include "architecture/x86_64/interrupt_service_routines.h"
#include "proc.h"

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
    FD_NONE = 0,
    FD_STDIN,
    FD_STDOUT,
    FD_PIPE_READ,
    FD_PIPE_WRITE,
    FD_FILE,
    FD_SOCKET,
    FD_UNIX,
    FD_EVENT,
    FD_TIMER,
    FD_EPOLL,
    FD_MEMFD,
} file_descriptor_type_t;

#define MAX_FDS 128

#define MAX_MMAP_REGIONS 128

typedef struct {
    uint64_t base;
    uint32_t pages;
    uint32_t prot;
    int handle;
    uint32_t file_page;
    uint8_t shared;
    uint8_t memfd_id;
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
} file_descriptor_slot_t;

typedef enum {
    PRIO_INTERACTIVE = 0,
    PRIO_BATCH = 1,
} prio_class_t;

#define SCHED_BATCH_THRESHOLD 10
#define SCHED_AGING_TICKS     100

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
    file_descriptor_slot_t fds[MAX_FDS];
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
    uint64_t max_rss_pages;
    uint64_t child_max_rss_pages;
    uint8_t idle_wait_depth;
    char cwd[PATH_MAX_LEN];
    char *env_block;
    uint32_t env_length;
    uint32_t env_count;
    uint64_t sig_handler[SIG_MAX + 1];
    uint64_t sig_restorer;
    uint32_t sig_pending;
    uint32_t sig_blocked;
    uint32_t sig_siginfo;
    int32_t  si_pid;
    int32_t  si_status;
    uint64_t si_addr;
    mmap_region_t mmaps[MAX_MMAP_REGIONS];
    int tgid;
    uint8_t is_thread;
    uint8_t exiting;
    char name[TASK_NAME_MAX];
} task_t;

void scheduler_init(void);

void scheduler_init_ap(int cpu_id);

void scheduler_tick_cpu(int cpu);

void scheduler_block_on(const void *chan, uint64_t deadline_ms, spinlock_t *lock, uint64_t *flags);

void scheduler_wake_all(const void *chan);

int scheduler_wake_n(const void *chan, int max);

uint64_t scheduler_event_sequence(void);

void scheduler_block_on_sequence(const void *chan, uint64_t deadline_ms, uint64_t expected_sequence);

extern const int scheduler_poll_channel;
#define SCHED_POLL_CHAN (&scheduler_poll_channel)

extern const int scheduler_keyboard_channel;
#define SCHED_SLEEP_CHAN (&scheduler_sleep_channel)
#define SCHED_KEYBOARD_CHAN (&scheduler_keyboard_channel)

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

int scheduler_set_env(task_t *t, const char *block, uint32_t len, uint32_t count);

void scheduler_set_task_name(task_t *t, const char *name);

void task_exit_with_signal(int sig) __attribute__((noreturn));

void scheduler_release_env(task_t *t);

unsigned int scheduler_set_alarm(task_t *t, unsigned int seconds);

int scheduler_release_shared_range(task_t *t, uint64_t start, uint64_t end);

void scheduler_region_forget_memfd(mmap_region_t *r);
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
int scheduler_fault_fill(uint64_t addr, uint64_t error_code, uint64_t user_rsp);

void scheduler_prefault_range(uint64_t addr, uint64_t len, int for_write);

task_t *task_spawn_thread(const char *name, task_t *leader, void (*entry)(void *arg), void *arg);

task_t *task_fork(uint64_t child_pml4, const isr_regs_t *regs);

void scheduler_kill_thread_group(task_t *t);

int scheduler_count_sharing_address_space(uint64_t pml4_phys);

task_t *scheduler_task_by_slot(int slot);

int scheduler_task_count(void);

int scheduler_live_task_count(void);
int scheduler_peak_live_tasks(void);
int scheduler_file_descriptor_high_water(int *which_task_out);

void scheduler_reap_slot(task_t *t);
void scheduler_dump_cpus(void);

void scheduler_reset_file_descriptors_to_std(task_t *t);

void file_descriptor_release(file_descriptor_slot_t *slot);
void file_descriptor_retain(const file_descriptor_slot_t *slot);
void scheduler_release_file_descriptors(task_t *t);
