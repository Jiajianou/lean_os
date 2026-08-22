/* kernel/acpi/acpi.h
 *
 * Just enough ACPI to find the MADT (Multiple APIC Description Table) and
 * read out of it what SMP bring-up (kernel/arch/x86_64/smp.c) actually
 * needs: the Local APIC's physical MMIO base, and the APIC id of every
 * enabled logical CPU. No AML interpreter, no other ACPI table, nothing
 * else this kernel doesn't have a use for yet.
 */
#pragma once

#include <stdint.h>

#include "arch/x86_64/cpu.h"

typedef struct {
    uint64_t lapic_base;
    int cpu_count; /* number of *enabled* Processor Local APIC entries found, capped at MAX_CPUS */
    uint32_t cpu_apic_ids[MAX_CPUS];
} acpi_madt_info_t;

/* Searches for the RSDP (BIOS EBDA + 0xE0000-0xFFFFF, the standard legacy
 * search range), walks the RSDT/XSDT it points at, and parses the MADT if
 * one is present. Returns 1 and fills *out on success; returns 0 (out
 * left untouched) if ACPI or the MADT isn't present at all - a legitimate
 * platform state (some minimal/non-ACPI machine), not corruption, so this
 * is a normal fallback-to-single-core signal, not a panic. */
int acpi_find_madt(acpi_madt_info_t *out);
