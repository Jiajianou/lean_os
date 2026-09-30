#include "lapic.h"

#include "drivers/kernel_log.h"
#include "interrupt_service_routines.h"
#include "memory_management/virtual_memory.h"
#include "timestamp_counter.h"

#define LAPIC_REG_ID       0x020u
#define LAPIC_REG_SVR      0x0F0u
#define LAPIC_REG_EOI      0x0B0u
#define LAPIC_REG_ICR_LOW  0x300u
#define LAPIC_REG_ICR_HIGH 0x310u
#define LAPIC_REG_LVT_TIMER     0x320u
#define LAPIC_REG_TIMER_INITIAL 0x380u
#define LAPIC_REG_TIMER_CURRENT 0x390u
#define LAPIC_REG_TIMER_DIVIDE  0x3E0u

#define LAPIC_TIMER_MASKED      (1u << 16)
#define LAPIC_TIMER_DIVIDE_BY_1 0xBu
#define LAPIC_TIMER_CALIBRATION_US 10000u

#define LAPIC_SVR_ENABLE       0x100u
#define LAPIC_ICR_DELIVERY_STATUS_PENDING (1u << 12)

static volatile uint32_t *lapic_mmio;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_mmio[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    lapic_mmio[reg / 4] = value;
}

void lapic_init(uint64_t phys_base) {
    virtual_memory_map_page(phys_base, phys_base, VIRTUAL_MEMORY_FLAG_WRITABLE);
    lapic_mmio = (volatile uint32_t *)(uintptr_t)phys_base;

    kernel_log_puts("[lapic] mapped at 0x");
    kernel_log_put_hex64(phys_base);
    kernel_log_putc('\n');

    lapic_init_this_cpu();
}

void lapic_init_this_cpu(void) {
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

uint32_t lapic_id(void) {
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_send_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

static void wait_for_icr_idle(void) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS_PENDING) {
        __asm__ volatile("pause");
    }
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector_and_flags) {
    wait_for_icr_idle();
    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, vector_and_flags);
    wait_for_icr_idle();
}

void lapic_send_ipi_all_excl_self(uint32_t vector_and_flags) {
    wait_for_icr_idle();
    lapic_write(LAPIC_REG_ICR_LOW, vector_and_flags | LAPIC_ICR_DEST_ALL_EXCL_SELF);
    wait_for_icr_idle();
}

/* The one-shot timer every core has. The 100 Hz tick decides when the
   kernel looks at the time, and every deadline a task slept to - a poll
   timeout, a futex wait, a frame's timerfd - used to be noticed on the
   first tick after it passed: up to ten milliseconds late, and a sixty
   hertz frame due at 16.7 ms woke at 20. Each core now arms its own timer
   for the earliest deadline it has been handed, and a wake-up lands where
   it was asked for. Counts per millisecond are measured against the TSC
   rather than assumed, because the timer's input clock is the bus or the
   crystal and differs between every machine this runs on. */
static uint64_t timer_counts_per_ms;

int lapic_timer_calibrate(void) {
    if (!lapic_mmio) {
        return 0;
    }
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_BY_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_MASKED | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INITIAL, 0xFFFFFFFFu);
    uint64_t start = tsc_read();
    while (tsc_to_us(tsc_read() - start) < LAPIC_TIMER_CALIBRATION_US) {
        __asm__ volatile("pause");
    }
    uint32_t left = lapic_read(LAPIC_REG_TIMER_CURRENT);
    uint64_t elapsed_us = tsc_to_us(tsc_read() - start);
    lapic_write(LAPIC_REG_TIMER_INITIAL, 0);
    uint64_t counted = 0xFFFFFFFFull - left;
    if (elapsed_us == 0 || counted < elapsed_us) {
        kernel_log_puts("[lapic] timer did not count - wake-ups stay on the tick.\n");
        return 0;
    }
    timer_counts_per_ms = counted * 1000u / elapsed_us;
    kernel_log_puts("[lapic] one-shot timer at ");
    kernel_log_put_dec((uint32_t)(timer_counts_per_ms / 1000u));
    kernel_log_puts(" counts/us - deadlines are met to the microsecond rather than the tick.\n");
    return 1;
}

int lapic_timer_available(void) {
    return timer_counts_per_ms != 0;
}

void lapic_timer_start_this_cpu(void) {
    if (!timer_counts_per_ms) {
        return;
    }
    lapic_write(LAPIC_REG_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_BY_1);
    lapic_write(LAPIC_REG_TIMER_INITIAL, 0);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_VECTOR);
}

void lapic_timer_arm_ns(uint64_t delay_ns) {
    if (!timer_counts_per_ms) {
        return;
    }
    uint64_t counts = delay_ns / 1000u * timer_counts_per_ms / 1000u;
    if (counts == 0) {
        counts = 1;
    }
    if (counts > 0xFFFFFFFFull) {
        counts = 0xFFFFFFFFull;
    }
    lapic_write(LAPIC_REG_TIMER_INITIAL, (uint32_t)counts);
}
