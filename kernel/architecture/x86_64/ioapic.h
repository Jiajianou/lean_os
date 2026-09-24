#pragma once

#include <stdint.h>

void ioapic_init(void);

int ioapic_available(void);

void ioapic_fall_back_to_pic(void);

void ioapic_route_irq(uint8_t irq, uint8_t lapic_id);

void ioapic_mask_irq(uint8_t irq);

void ioapic_count_irq(uint8_t vector, int cpu);
uint64_t ioapic_irq_count(uint8_t vector, int cpu);
