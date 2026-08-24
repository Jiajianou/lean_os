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

/* M47: tells this module where the firmware said the RSDP is, before
 * anything else here is called. Under UEFI that pointer comes from the
 * system table's configuration array (kernel/boot/uefi/boot.c) and is
 * *not* discoverable by the legacy EBDA/0xE0000 scan below - which is why
 * ACPI had found nothing at all, on every boot, since M26 made UEFI the
 * only boot path. Passing 0 (or never calling this) leaves the legacy
 * scan as the only source, exactly as before. */
void acpi_set_rsdp(uint64_t phys);

/* Searches for the RSDP (BIOS EBDA + 0xE0000-0xFFFFF, the standard legacy
 * search range), walks the RSDT/XSDT it points at, and parses the MADT if
 * one is present. Returns 1 and fills *out on success; returns 0 (out
 * left untouched) if ACPI or the MADT isn't present at all - a legitimate
 * platform state (some minimal/non-ACPI machine), not corruption, so this
 * is a normal fallback-to-single-core signal, not a panic. */
int acpi_find_madt(acpi_madt_info_t *out);

/* M47: what the FADT says about turning the machine off and restarting
 * it - the same RSDT/XSDT walk acpi_find_madt already does, one more
 * table along. Everything here is "0 means this platform didn't tell us",
 * because kernel/power/power.c is built to fall through to its next tier
 * rather than to require any of it.
 *
 * The `\_S5` sleep type properly lives in AML in the DSDT, and writing an
 * AML parser is not in scope for this project - so slp_typ_a/b are NOT
 * from the firmware. power.c supplies the well-known values instead and
 * says so in the log; see acpi_find_power's own comment. */
typedef struct {
    uint32_t pm1a_cnt;    /* PM1a control register port, or 0 */
    uint32_t pm1b_cnt;    /* PM1b control register port, or 0 if the platform has only one */
    uint32_t smi_cmd;     /* SMI command port for the ACPI-enable handshake, or 0 if none is needed */
    uint8_t acpi_enable;  /* value to write to smi_cmd to hand control of the PM registers to the OS */
    uint32_t reset_port;  /* the FADT reset register, if it is a SystemIO one and the table says it is supported; 0 otherwise */
    uint8_t reset_value;
} acpi_power_info_t;

/* Returns 1 and fills *out if a FADT was found (fields the FADT didn't
 * supply are left 0), or 0 if there is no ACPI/FADT at all - the same
 * "a legitimate platform state, not corruption" contract acpi_find_madt
 * has. */
int acpi_find_power(acpi_power_info_t *out);
