#pragma once

#include <stdint.h>

#define LAPIC_ICR_DELIVERY_FIXED   0x00000u
#define LAPIC_ICR_DELIVERY_NMI     0x00400u
#define LAPIC_ICR_DELIVERY_INIT    0x00500u
#define LAPIC_ICR_DELIVERY_STARTUP 0x00600u
#define LAPIC_ICR_LEVEL_ASSERT     0x04000u
#define LAPIC_ICR_DEST_ALL_EXCL_SELF 0xC0000u

void lapic_init(uint64_t phys_base);
void lapic_init_this_cpu(void);

uint32_t lapic_id(void);
void lapic_send_eoi(void);

void lapic_send_ipi(uint32_t apic_id, uint32_t vector_and_flags);
void lapic_send_ipi_all_excl_self(uint32_t vector_and_flags);
