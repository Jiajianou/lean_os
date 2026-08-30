#include "pit.h"

#include "sched/sched.h"

#include "arch/x86_64/io.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "pcspk.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ     1193182u /* fixed 8253/8254 input clock, Hz */

#define PIT_CMD_CHANNEL0     0x00
#define PIT_CMD_LOHI         0x30 /* access mode: low byte then high byte */
#define PIT_CMD_MODE3_SQUARE 0x06
#define PIT_IRQ 0

static volatile uint64_t ticks;
static void (*tick_hook)(void);

static void pit_irq(isr_regs_t *regs) {
    (void)regs;
    ticks++;
    /* M62: the speaker's own deadline. One comparison per tick, and the
     * reason a tone does not block whoever asked for it - see pcspk.h.
     * Ahead of the scheduler hook deliberately: a tick that ends in a
     * context switch never comes back here. */
    pcspk_tick();
    if (tick_hook) {
        tick_hook();
    }
}

void pit_init(void) {
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / PIT_HZ);

    outb(PIT_COMMAND, PIT_CMD_CHANNEL0 | PIT_CMD_LOHI | PIT_CMD_MODE3_SQUARE);
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);

    irq_register_handler(PIT_IRQ, pit_irq);
    pic_clear_mask(PIT_IRQ);
}

uint64_t pit_get_ticks(void) {
    return ticks;
}

void pit_sleep_ms(uint32_t ms) {
    uint64_t needed = ((uint64_t)ms * PIT_HZ + 999) / 1000; /* round up */
    if (needed == 0) {
        needed = 1;
    }
    uint64_t target = ticks + needed;
    /* M68: this loop is the CPU halted, waiting for the clock. Saying so
     * is what makes the idle measurement mean anything during boot, when
     * the machine spends most of its time right here and the task doing
     * it is not the idle identity yet. */
    /* ---- M68: this deliberately HALTS rather than blocking -------------
     *
     * sched_sleep_until exists and this was the obvious place to use it:
     * a task waiting for the clock is the definition of a task with
     * nothing to do, so it should leave the run queue. It was implemented,
     * measured, and taken back out, and the measurement is the reason.
     *
     * A blocked task gives up the CPU and has to **queue to get it back**.
     * A halted one does not - the timer interrupt resumes it in place. So
     * blocking adds a full scheduling round-trip to every sleep, and this
     * kernel's boot performs 138 of them plus polling loops that call
     * this hundreds of times. Measured: a self-test that polls
     * `pit_sleep_ms(10)` 300 times took 3 seconds halting and **15.6
     * seconds blocking**, and the boot failed wherever some other test's
     * budget ran out first.
     *
     * The idle accounting still works, because it never depended on
     * blocking: sched_idle_enter/exit brackets say "this CPU is halted,
     * waiting for time to pass", which is exactly what is happening here
     * and is what the [m68] measurement counts.
     *
     * The general lesson, and it is the one M68 kept relearning: **not
     * every wait should become a sleep.** A wait that is already cheap -
     * one instruction, resumed by the interrupt it is waiting for - gets
     * more expensive when you make it a scheduling decision. Blocking
     * pays when the alternative is spinning through schedule(), which is
     * what SYS_waitfds and the pipe waits do, and not otherwise. */
    sched_idle_enter();
    while (ticks < target) {
        __asm__ volatile("hlt");
    }
    sched_idle_exit();
}

void pit_set_tick_hook(void (*hook)(void)) {
    tick_hook = hook;
}
