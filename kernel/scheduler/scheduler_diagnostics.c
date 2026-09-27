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
void scheduler_diagnostics_toggle(void) {
    diagnostics_showing = !diagnostics_showing;
    if (diagnostics_showing) {
        scheduler_paint_diagnostics();
        signal_compositor(SIGSTOP);
    } else {
        kernel_log_puts("[diagnostics] Ctrl+Alt+D - overlay off, compositor continued.\n");
        signal_compositor(SIGCONT);
    }
}

void scheduler_diagnostics_tick(uint64_t ticks) {
    if (diagnostics_showing && ticks % (2 * PIT_HZ) == 0) {
        scheduler_paint_diagnostics();
    }
}
