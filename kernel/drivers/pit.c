#include "pit.h"

#include "scheduler/scheduler.h"

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/pic.h"
#include "pc_speaker.h"
#include "profile/sampler.h"
#include "xhci.h"
#include "scheduler/scheduler_diagnostics.h"
#include "architecture/x86_64/timestamp_counter.h"
#include "tick_clock.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ     1193182u

#define PIT_COMMAND_CHANNEL0     0x00
#define PIT_COMMAND_LOHI         0x30
#define PIT_COMMAND_MODE3_SQUARE 0x06
#define PIT_IRQ 0

#define PIT_CHANNEL2_DATA        0x42
#define PIT_COMMAND_CHANNEL2     0x80
#define PIT_COMMAND_MODE0        0x00
#define PIT_GATE_PORT            0x61
#define PIT_GATE_CHANNEL2        0x01
#define PIT_GATE_SPEAKER_DATA    0x02
#define PIT_GATE_CHANNEL2_OUT    0x20

static volatile uint64_t ticks;
static volatile uint64_t interrupts;
static volatile uint64_t echoes;
static volatile uint64_t last_tick_tsc;
static tick_clock_t tick_clock;
static void (*tick_hook)(void);

static void pit_irq(isr_regs_t *regs) {
    uint64_t started = tsc_read();
    interrupts++;
    /* An interrupt the TSC says is too early to be the next tick is the
       controller's echo of the last one (tick_clock.h) - taken and
       acknowledged, but not time. */
    if (!tick_clock_accept(&tick_clock, ticks, started)) {
        echoes++;
        interrupt_time_account(tsc_read() - started);
        return;
    }
    last_tick_tsc = started;
    ticks++;
    profile_sample(regs);
    scheduler_account_tick((regs->cs & 3) != 0);
    pc_speaker_tick();
    xhci_poll();
    scheduler_diagnostics_tick(ticks);
    interrupt_time_account(tsc_read() - started);
    if (tick_hook) {
        tick_hook();
    }
}

void pit_init(void) {
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / PIT_HZ);

    outb(PIT_COMMAND, PIT_COMMAND_CHANNEL0 | PIT_COMMAND_LOHI | PIT_COMMAND_MODE3_SQUARE);
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);

    irq_register_handler(PIT_IRQ, pit_irq);
    irq_enable_line(PIT_IRQ);
}

uint64_t pit_get_ticks(void) {
    return ticks;
}

uint64_t pit_get_interrupts(void) {
    return interrupts;
}

uint64_t pit_get_echoes(void) {
    return echoes;
}

static uint64_t interrupts_off(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupts_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* Channel 2 counted down once from `counts` in mode 0, with OUT read back
   through port 0x61 - the timer's own oscillator, and no interrupt anywhere
   in it. The load and the first TSC read are one step with interrupts off,
   and so is each poll with the TSC read after it, so an interrupt handler
   can delay the poll that sees OUT rise but never sit between that poll and
   its timestamp. The patience is 2^33 TSC cycles - seconds at any rate a
   TSC runs at, against a window of tens of milliseconds - so a channel that
   never answers (no 8254, or one whose clock is gated) is given up on
   rather than hung on. */
int pit_channel2_cycles(uint16_t counts, uint64_t *cycles) {
    uint64_t flags = interrupts_off();
    uint8_t saved = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (uint8_t)((saved & ~PIT_GATE_SPEAKER_DATA) | PIT_GATE_CHANNEL2));
    outb(PIT_COMMAND, PIT_COMMAND_CHANNEL2 | PIT_COMMAND_LOHI | PIT_COMMAND_MODE0);
    outb(PIT_CHANNEL2_DATA, (uint8_t)(counts & 0xFF));
    outb(PIT_CHANNEL2_DATA, (uint8_t)(counts >> 8));
    uint64_t start = tsc_read();
    interrupts_restore(flags);

    int answered = 0;
    uint64_t end = start;
    for (;;) {
        flags = interrupts_off();
        uint8_t gate = inb(PIT_GATE_PORT);
        end = tsc_read();
        interrupts_restore(flags);
        if (gate & PIT_GATE_CHANNEL2_OUT) {
            answered = 1;
            break;
        }
        if (end - start > (1ull << 33)) {
            break;
        }
    }

    flags = interrupts_off();
    outb(PIT_GATE_PORT, saved);
    interrupts_restore(flags);
    if (!answered) {
        return 0;
    }
    *cycles = end - start;
    return 1;
}

void pit_start_tick_clock(uint64_t window_cycles, uint32_t window_counts) {
    if (window_counts == 0) {
        return;
    }
    uint64_t divisor = PIT_BASE_FREQ / PIT_HZ;
    uint64_t flags = interrupts_off();
    tick_clock.epoch_tsc = last_tick_tsc ? last_tick_tsc : tsc_read();
    tick_clock.epoch_ticks = ticks;
    tick_clock.cycles_per_tick = window_cycles * divisor / window_counts;
    interrupts_restore(flags);
}

void pit_sleep_ms(uint32_t ms) {
    uint64_t needed = ((uint64_t)ms * PIT_HZ + 999) / 1000;
    if (needed == 0) {
        needed = 1;
    }
    uint64_t target = ticks + needed;
    scheduler_idle_enter();
    while (ticks < target) {
        scheduler_halt();
    }
    scheduler_idle_exit();
}

void pit_set_tick_hook(void (*hook)(void)) {
    tick_hook = hook;
}
