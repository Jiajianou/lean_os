#include "pit.h"

#include "scheduler/scheduler.h"

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/pic.h"
#include "pc_speaker.h"
#include "profile/sampler.h"
#include "xhci.h"
#include "scheduler/scheduler_diagnostics.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ     1193182u

#define PIT_COMMAND_CHANNEL0     0x00
#define PIT_COMMAND_LOHI         0x30
#define PIT_COMMAND_MODE3_SQUARE 0x06
#define PIT_IRQ 0

static volatile uint64_t ticks;
static void (*tick_hook)(void);

static void pit_irq(isr_regs_t *regs) {
    ticks++;
    profile_sample(regs);
    scheduler_account_tick((regs->cs & 3) != 0);
    pc_speaker_tick();
    xhci_poll();
    if (tick_hook) {
        tick_hook();
    }
    scheduler_diagnostics_tick(ticks);
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

void pit_sleep_ms(uint32_t ms) {
    uint64_t needed = ((uint64_t)ms * PIT_HZ + 999) / 1000;
    if (needed == 0) {
        needed = 1;
    }
    uint64_t target = ticks + needed;
    scheduler_idle_enter();
    while (ticks < target) {
        __asm__ volatile("hlt");
    }
    scheduler_idle_exit();
}

void pit_set_tick_hook(void (*hook)(void)) {
    tick_hook = hook;
}
