#pragma once

#include <stdint.h>

#include "arch/x86_64/cpu.h"

#define MAX_IOAPICS 4
#define MAX_IRQ_OVERRIDES 16

typedef struct {
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
} acpi_ioapic_t;

typedef struct {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} acpi_irq_override_t;

typedef struct {
    uint64_t lapic_base;
    int cpu_count;
    uint32_t cpu_apic_ids[MAX_CPUS];
    int ioapic_count;
    acpi_ioapic_t ioapics[MAX_IOAPICS];
    int override_count;
    acpi_irq_override_t overrides[MAX_IRQ_OVERRIDES];
} acpi_madt_info_t;

void acpi_set_rsdp(uint64_t phys);

int acpi_find_madt(acpi_madt_info_t *out);

typedef struct {
    uint32_t pm1a_cnt;
    uint32_t pm1b_cnt;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint32_t reset_port;
    uint8_t reset_value;
} acpi_power_info_t;

int acpi_find_power(acpi_power_info_t *out);

int acpi_find_s5(uint8_t *slp_a, uint8_t *slp_b);
