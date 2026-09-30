#include "scheduler_diagnostics.h"

#include "architecture/x86_64/cpu.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "drivers/diagnostic_overlay.h"
#include "drivers/kernel_log.h"
#include "drivers/i2c_touchpad.h"
#include "drivers/pit.h"
#include "scheduler/scheduler.h"
#include "signal.h"
#include "library/kernel_library.h"

typedef struct {
    char text[96];
    uint32_t length;
} line_t;

static void line_text(line_t *line, const char *text) {
    while (*text && line->length + 1 < sizeof(line->text)) {
        line->text[line->length++] = *text++;
    }
    line->text[line->length] = 0;
}

static void line_decimal(line_t *line, uint64_t value) {
    char digits[21];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < 20);
    char text[21];
    for (int i = 0; i < count; i++) {
        text[i] = digits[count - 1 - i];
    }
    text[count] = 0;
    line_text(line, text);
}

static void line_hex(line_t *line, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char text[19];
    int count = 0;
    text[count++] = '0';
    text[count++] = 'x';
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint32_t nibble = (uint32_t)(value >> shift) & 0xF;
        if (nibble || started || shift == 0) {
            text[count++] = digits[nibble];
            started = 1;
        }
    }
    text[count] = 0;
    line_text(line, text);
}

static const char *state_name(int state) {
    switch (state) {
        case TASK_READY: return "ready  ";
        case TASK_RUNNING: return "RUNNING";
        case TASK_TERMINATED: return "exited ";
        case TASK_BLOCKED: return "blocked";
        case TASK_STOPPED: return "stopped";
        default: return "?      ";
    }
}

void scheduler_paint_diagnostics(void) {
    kernel_log_puts("[diagnostics] Ctrl+Alt+D - painting the task list over the screen.\n");
    diagnostic_overlay_begin();

    line_t line = {{0}, 0};
    line_text(&line, "lean_os kernel - Ctrl+Alt+D. timer ticks ");
    line_decimal(&line, pit_get_ticks());
    diagnostic_overlay_line(line.text);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        task_t *current = scheduler_cpu_current(cpu);
        if (!current) {
            continue;
        }
        line_t row = {{0}, 0};
        line_text(&row, "cpu ");
        line_decimal(&row, (uint64_t)cpu);
        line_text(&row, ": ");
        line_text(&row, current->name[0] ? current->name : "(unnamed)");
        line_text(&row, " pid ");
        line_decimal(&row, (uint64_t)current->id);
        line_text(&row, " ticks ");
        line_decimal(&row, scheduler_total_ticks(cpu));
        line_text(&row, " idle ");
        line_decimal(&row, scheduler_idle_ticks(cpu));
        diagnostic_overlay_line(row.text);
    }
    if (i2c_touchpad_present()) {
        i2c_touchpad_statistics_t pad;
        i2c_touchpad_statistics(&pad);
        line_t row = {{0}, 0};
        line_text(&row, "touchpad polls ");
        line_decimal(&row, pad.polls);
        line_text(&row, " fail ");
        line_decimal(&row, pad.read_failures);
        line_text(&row, " empty ");
        line_decimal(&row, pad.empty);
        line_text(&row, " other-id ");
        line_decimal(&row, pad.wrong_report);
        line_text(&row, " reports ");
        line_decimal(&row, pad.reports);
        line_text(&row, " moved ");
        line_decimal(&row, pad.injected);
        diagnostic_overlay_line(row.text);
        line_t raw = {{0}, 0};
        line_text(&raw, "touchpad last length ");
        line_decimal(&raw, pad.last_length);
        line_text(&raw, " id wanted ");
        line_decimal(&raw, pad.report_id);
        line_text(&raw, " bytes");
        for (int i = 0; i < 8; i++) {
            line_text(&raw, " ");
            line_hex(&raw, pad.last_bytes[i]);
        }
        diagnostic_overlay_line(raw.text);
    }
    diagnostic_overlay_line("pid state   user+sys  wait        name");

    int slots = scheduler_task_slot_count();
    for (int i = 0; i < slots; i++) {
        task_t *t = scheduler_task_slot(i);
        if (!t || t->state == TASK_FREE || t->state == TASK_TERMINATED) {
            continue;
        }
        line_t row = {{0}, 0};
        line_decimal(&row, (uint64_t)t->id);
        line_text(&row, " ");
        line_text(&row, state_name(t->state));
        line_text(&row, " ");
        line_decimal(&row, t->user_ticks + t->sys_ticks);
        line_text(&row, " ");
        if (t->state == TASK_BLOCKED) {
            line_hex(&row, (uint64_t)(uintptr_t)t->wait_chan);
        } else {
            line_text(&row, "-");
        }
        line_text(&row, " ");
        line_text(&row, t->name[0] ? t->name : "(unnamed)");
        diagnostic_overlay_line(row.text);
    }
}

static volatile int diagnostics_showing;

/* Sticky, because a desktop that is slow rather than dead repaints over a single
   frame of it before anybody can photograph it. */
static void signal_compositor(int sig) {
    int slots = scheduler_task_slot_count();
    for (int i = 0; i < slots; i++) {
        task_t *t = scheduler_task_slot(i);
        if (t && t->state != TASK_FREE && t->state != TASK_TERMINATED &&
            k_strcmp(t->name, "compositor") == 0) {
            scheduler_raise_signal(t, sig);
        }
    }
}

/* The compositor is stopped rather than raced: it repaints over anything the
   kernel draws. Painting comes first, so a CPU wedged on the scheduler lock
   still leaves a picture behind. */
void scheduler_log_task_table(void);

void scheduler_diagnostics_toggle(void) {
    diagnostics_showing = !diagnostics_showing;
    if (diagnostics_showing) {
        scheduler_paint_diagnostics();
        scheduler_log_task_table();
        signal_compositor(SIGSTOP);
    } else {
        kernel_log_puts("[diagnostics] Ctrl+Alt+D - overlay off, compositor continued.\n");
        signal_compositor(SIGCONT);
    }
}

void scheduler_hang_check(void);

void scheduler_diagnostics_tick(uint64_t ticks) {
    if (diagnostics_showing && ticks % (2 * PIT_HZ) == 0) {
        scheduler_paint_diagnostics();
    }
    if (ticks % PIT_HZ == 0) {
        scheduler_hang_check();
    }
}

/* M199: the hang detector. The laptop froze with the kernel still writing
   its log, and a QEMU run with eight cores went silent four seconds into a
   browser launch - and neither said what any core was doing. Once a second
   the timer on cpu 0 looks for the three shapes a hang takes, and says which
   one, where, and who is involved, into the same log the stick carries:

     a core that has taken no tick for two seconds has interrupts off - it
       is spinning on a lock or looping in a handler. It is sent an NMI and
       reports its own instruction pointer and the kernel return addresses
       on its stack;
     a core that has run one task in the kernel for five seconds without a
       switch is looping in a system call. Same NMI, same report;
     a task that has waited ten seconds for a sleep lock is named with the
       lock, its holder, and what the holder is doing.

   Each is reported once per episode, so a machine that is merely slow gets
   one paragraph rather than a log full of them. The return addresses are
   resolved on the host with nm against build/kernel.elf. */
extern char __kernel_text_start[];
extern char __kernel_text_end[];

static uint32_t hang_tickless_seconds = 3u;
static uint32_t hang_kernel_seconds = 5u;
static uint32_t hang_lock_wait_ms = 10000u;
static hang_statistics_t statistics;
#define HANG_STACK_WORDS        512u
#define HANG_RETURNS_SHOWN      16u

static uint64_t seen_ticks[MAX_CPUS];
static uint64_t seen_switches[MAX_CPUS];
static uint64_t seen_sys_ticks[MAX_CPUS];
static uint32_t tickless_seconds[MAX_CPUS];
static uint32_t kernel_seconds[MAX_CPUS];
static uint8_t reported_tickless[MAX_CPUS];
static uint8_t reported_kernel_loop[MAX_CPUS];
static volatile uint8_t report_requested[MAX_CPUS];
#define LOCK_REPORTS_KEPT 8
static struct {
    const void *lock;
    int holder;
} lock_reports[LOCK_REPORTS_KEPT];
static uint32_t lock_reports_next;

static void log_task(const task_t *t);

#define HANG_READY_MS 2000u
#define HANG_STARVED_REPORT_INTERVAL_MS 10000u
static uint64_t last_starved_report_ms;

static void check_starved_tasks(uint64_t now_ms) {
    if (last_starved_report_ms != 0 && now_ms - last_starved_report_ms < HANG_STARVED_REPORT_INTERVAL_MS) {
        return;
    }
    int slots = scheduler_task_slot_count();
    for (int i = 0; i < slots; i++) {
        task_t *t = scheduler_task_slot(i);
        if (!t || t->state != TASK_READY || t->is_idle || t->last_ran_ms == 0 ||
            now_ms - t->last_ran_ms < HANG_READY_MS || t->starvation_reported) {
            continue;
        }
        t->starvation_reported = 1;
        last_starved_report_ms = now_ms;
        statistics.starved++;
        kernel_log_puts("[hang] ");
        log_task(t);
        kernel_log_puts(" has been ready for ");
        kernel_log_put_dec((uint32_t)((now_ms - t->last_ran_ms) / 1000u));
        kernel_log_puts(" s and no core has run it; bound to cpu ");
        if (t->home_cpu < 0) {
            kernel_log_puts("none");
        } else {
            kernel_log_put_dec((uint32_t)t->home_cpu);
        }
        kernel_log_puts(", priority ");
        kernel_log_put_dec(t->prio);
        kernel_log_putc('\n');
        return;
    }
}

static int is_kernel_text(uint64_t address) {
    return address >= (uint64_t)(uintptr_t)__kernel_text_start &&
           address < (uint64_t)(uintptr_t)__kernel_text_end;
}

static void log_task(const task_t *t) {
    kernel_log_puts(t && t->name[0] ? t->name : "(unnamed)");
    kernel_log_puts(" pid ");
    kernel_log_put_dec((uint32_t)(t ? t->id : -1));
}

void scheduler_log_kernel_returns(uint64_t from, uint64_t stack_top) {
    kernel_log_puts("  kernel return addresses:");
    if (from == 0 || stack_top == 0 || from >= stack_top) {
        kernel_log_puts(" (no stack to read)\n");
        return;
    }
    uint32_t shown = 0;
    for (uint64_t at = from & ~7ull, words = 0;
         at + 8 <= stack_top && words < HANG_STACK_WORDS && shown < HANG_RETURNS_SHOWN;
         at += 8, words++) {
        uint64_t word = *(const volatile uint64_t *)(uintptr_t)at;
        if (is_kernel_text(word)) {
            kernel_log_puts(" 0x");
            kernel_log_put_hex32((uint32_t)word);
            shown++;
        }
    }
    kernel_log_puts(shown ? "\n" : " none\n");
}

static void log_blocked_task(const task_t *t) {
    kernel_log_puts("  ");
    log_task(t);
    kernel_log_puts(" state ");
    kernel_log_puts(state_name(t->state));
    kernel_log_puts(" wait 0x");
    kernel_log_put_hex64((uint64_t)(uintptr_t)t->wait_chan);
    kernel_log_puts(" locks held ");
    kernel_log_put_dec(t->sleep_locks_held);
    kernel_log_putc('\n');
    if (t->state == TASK_BLOCKED || t->state == TASK_READY) {
        scheduler_log_kernel_returns(t->rsp, t->kernel_stack_top);
    }
}

int scheduler_hang_report_requested(int cpu) {
    return cpu >= 0 && cpu < MAX_CPUS && report_requested[cpu];
}

void scheduler_hang_set_thresholds(uint32_t tickless_seconds, uint32_t kernel_seconds,
                                   uint32_t lock_wait_ms) {
    hang_tickless_seconds = tickless_seconds;
    hang_kernel_seconds = kernel_seconds;
    hang_lock_wait_ms = lock_wait_ms;
}

void scheduler_hang_statistics(hang_statistics_t *out) {
    *out = statistics;
}

void scheduler_hang_report_this_cpu(int cpu, uint64_t rip, uint64_t rsp, int user_mode) {
    report_requested[cpu] = 0;
    statistics.answers++;
    statistics.last_answer_rip = rip;
    task_t *t = scheduler_cpu_current(cpu);
    kernel_log_puts("[hang] cpu ");
    kernel_log_put_dec((uint32_t)cpu);
    kernel_log_puts(" answers: running ");
    log_task(t);
    kernel_log_puts(user_mode ? " in user mode" : " in the kernel");
    kernel_log_puts(" at rip 0x");
    kernel_log_put_hex64(rip);
    kernel_log_putc('\n');
    if (!user_mode) {
        scheduler_log_kernel_returns(rsp, t ? t->kernel_stack_top : 0);
    }
}

static void ask_cpu_to_report(int cpu) {
    report_requested[cpu] = 1;
    smp_send_nmi(cpu);
}

int scheduler_hang_lock_was_reported(const void *lock, int holder) {
    for (int i = 0; i < LOCK_REPORTS_KEPT; i++) {
        if (lock_reports[i].lock == lock && lock_reports[i].holder == holder) {
            return 1;
        }
    }
    return 0;
}

/* Each wait is reported once, however long it lasts, and every waiter past
   the detector's patience is reported - two locks contended at once used to
   take turns, a paragraph a second each. */
#define LOCK_REPORTS_PER_CHECK 3

static void check_lock_waits(uint64_t now_ms) {
    int slots = scheduler_task_slot_count();
    int reported = 0;
    for (int i = 0; i < slots && reported < LOCK_REPORTS_PER_CHECK; i++) {
        task_t *t = scheduler_task_slot(i);
        const void *waiting = t ? t->sleep_lock_waiting : (const void *)0;
        if (!t || t->state != TASK_BLOCKED || !waiting || t->lock_wait_reported ||
            now_ms - t->sleep_lock_wait_since_ms < hang_lock_wait_ms) {
            continue;
        }
        const sleep_lock_t *lock = (const sleep_lock_t *)waiting;
        t->lock_wait_reported = 1;
        reported++;
        statistics.lock_waits++;
        statistics.last_lock_holder = lock->holder;
        lock_reports[lock_reports_next % LOCK_REPORTS_KEPT].lock = lock;
        lock_reports[lock_reports_next % LOCK_REPORTS_KEPT].holder = lock->holder;
        lock_reports_next++;
        kernel_log_puts("[hang] ");
        log_task(t);
        kernel_log_puts(" has waited ");
        kernel_log_put_dec((uint32_t)((now_ms - t->sleep_lock_wait_since_ms) / 1000u));
        kernel_log_puts(" s for the sleep lock at 0x");
        kernel_log_put_hex64((uint64_t)(uintptr_t)lock);
        kernel_log_puts(", held by pid ");
        kernel_log_put_dec((uint32_t)lock->holder);
        kernel_log_puts(" with ");
        kernel_log_put_dec((uint32_t)lock->waiters);
        kernel_log_puts(" waiting\n");
        task_t *holder = 0;
        for (int j = 0; j < slots; j++) {
            task_t *h = scheduler_task_slot(j);
            if (h && h->state != TASK_FREE && h->id == lock->holder) {
                holder = h;
            }
        }
        if (!holder) {
            kernel_log_puts("  the holder no longer exists - the lock was never released\n");
        } else {
            log_blocked_task(holder);
            for (int c = 0; c < MAX_CPUS; c++) {
                if (scheduler_cpu_current(c) == holder) {
                    ask_cpu_to_report(c);
                }
            }
        }
        log_blocked_task(t);
    }
}

static volatile int hang_check_running;

void scheduler_hang_check(void) {
    int self = smp_current_cpu();
    if (self != 0 || __atomic_exchange_n(&hang_check_running, 1, __ATOMIC_ACQUIRE)) {
        return;
    }
    int cpus = smp_cpu_count > 0 && smp_cpu_count <= MAX_CPUS ? smp_cpu_count : 1;
    for (int c = 0; c < cpus; c++) {
        task_t *t = scheduler_cpu_current(c);
        if (!t) {
            continue;
        }
        uint64_t ticks = scheduler_total_ticks(c);
        uint64_t switches = scheduler_switch_count(c);
        if (c != self && ticks == seen_ticks[c] && switches == seen_switches[c]) {
            if (++tickless_seconds[c] >= hang_tickless_seconds && !reported_tickless[c]) {
                reported_tickless[c] = 1;
                statistics.tickless++;
                kernel_log_puts("[hang] cpu ");
                kernel_log_put_dec((uint32_t)c);
                kernel_log_puts(" has taken no timer tick for ");
                kernel_log_put_dec(tickless_seconds[c]);
                kernel_log_puts(" s - interrupts are off there; last seen running ");
                log_task(t);
                kernel_log_putc('\n');
                ask_cpu_to_report(c);
            }
        } else {
            tickless_seconds[c] = 0;
            reported_tickless[c] = 0;
        }
        uint64_t sys = t->sys_ticks;
        if (!t->is_idle && switches == seen_switches[c] && ticks != seen_ticks[c] &&
            sys - seen_sys_ticks[c] + 10 >= ticks - seen_ticks[c]) {
            if (++kernel_seconds[c] >= hang_kernel_seconds && !reported_kernel_loop[c]) {
                reported_kernel_loop[c] = 1;
                statistics.kernel_loops++;
                kernel_log_puts("[hang] cpu ");
                kernel_log_put_dec((uint32_t)c);
                kernel_log_puts(" has run ");
                log_task(t);
                kernel_log_puts(" in the kernel for ");
                kernel_log_put_dec(kernel_seconds[c]);
                kernel_log_puts(" s without a switch\n");
                if (c == self) {
                    report_requested[c] = 0;
                } else {
                    ask_cpu_to_report(c);
                }
            }
        } else {
            kernel_seconds[c] = 0;
            reported_kernel_loop[c] = 0;
        }
        seen_ticks[c] = ticks;
        seen_switches[c] = switches;
        seen_sys_ticks[c] = sys;
    }
    check_lock_waits(clock_monotonic_ms());
    check_starved_tasks(clock_monotonic_ms());
    __atomic_store_n(&hang_check_running, 0, __ATOMIC_RELEASE);
}

/* M199: the same table as the overlay, into the log - the overlay is gone
   the moment the key is pressed again, and on the laptop the log on the stick
   is what comes home. Every live task, what it is waiting for, what it has
   pending, and for one that is not running, where in the kernel it sleeps. */
void scheduler_log_task_table(void) {
    kernel_log_puts("[hang] task table:\n");
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        task_t *current = scheduler_cpu_current(cpu);
        if (!current) {
            continue;
        }
        kernel_log_puts("  cpu ");
        kernel_log_put_dec((uint32_t)cpu);
        kernel_log_puts(" runs ");
        log_task(current);
        kernel_log_putc('\n');
    }
    int slots = scheduler_task_slot_count();
    for (int i = 0; i < slots; i++) {
        task_t *t = scheduler_task_slot(i);
        if (!t || t->state == TASK_FREE || t->is_idle) {
            continue;
        }
        kernel_log_puts("  ");
        log_task(t);
        kernel_log_puts(" tgid ");
        kernel_log_put_dec((uint32_t)t->tgid);
        kernel_log_puts(" ");
        kernel_log_puts(state_name(t->state));
        kernel_log_puts(" ticks ");
        kernel_log_put_dec((uint32_t)t->user_ticks);
        kernel_log_puts("u+");
        kernel_log_put_dec((uint32_t)t->sys_ticks);
        kernel_log_puts("s wait 0x");
        kernel_log_put_hex64((uint64_t)(uintptr_t)t->wait_chan);
        kernel_log_puts(" deadline ");
        kernel_log_put_dec((uint32_t)t->wake_deadline_ms);
        kernel_log_puts(" signal ");
        kernel_log_put_dec((uint32_t)t->pending_signal);
        kernel_log_puts(" pending 0x");
        kernel_log_put_hex32(t->sig_pending);
        kernel_log_puts(" locks ");
        kernel_log_put_dec(t->sleep_locks_held);
        if (t->exiting) {
            kernel_log_puts(" exiting");
        }
        kernel_log_putc('\n');
        if (t->state == TASK_BLOCKED || t->state == TASK_STOPPED) {
            scheduler_log_kernel_returns(t->rsp, t->kernel_stack_top);
        }
    }
}
