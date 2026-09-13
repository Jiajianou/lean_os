#include "lapic.h"

#include "drivers/kernel_log.h"
#include "interrupt_service_routines.h"
#include "memory_management/virtual_memory.h"

#define LAPIC_REG_ID       0x020u
#define LAPIC_REG_SVR      0x0F0u
#define LAPIC_REG_EOI      0x0B0u
#define LAPIC_REG_ICR_LOW  0x300u
#define LAPIC_REG_ICR_HIGH 0x310u

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
