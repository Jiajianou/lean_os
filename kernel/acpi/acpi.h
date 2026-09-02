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

/* ---- M103: the I/O APIC entries this parser used to skip -------------
 *
 * The MADT's comment said I/O APIC and Interrupt Source Override entries
 * were "out of scope for the minimal 'start every enabled core' job".
 * They are the scope now: routing a legacy line through the I/O APIC
 * needs the first, and getting the RIGHT line needs the second.
 *
 * **The overrides are the part that is not optional.** The ISA timer is
 * IRQ 0 by convention and is routed to global system interrupt 2 on
 * essentially every PC ever built - so a kernel that programmed GSI 0
 * for the timer would program a line nothing is connected to and get no
 * ticks. That override is not an oddity; it is the normal case, and it
 * is why the MADT has the entry type at all.
 */
#define MAX_IOAPICS 4
#define MAX_IRQ_OVERRIDES 16

typedef struct {
    uint8_t id;
    uint32_t address;   /* physical, and inside the identity map */
    uint32_t gsi_base;  /* the first global system interrupt this one serves */
} acpi_ioapic_t;

typedef struct {
    uint8_t source;     /* the ISA IRQ number a driver knows */
    uint32_t gsi;       /* the global system interrupt it is actually on */
    uint16_t flags;     /* polarity and trigger mode, MADT-encoded */
} acpi_irq_override_t;

typedef struct {
    uint64_t lapic_base;
    int cpu_count; /* number of *enabled* Processor Local APIC entries found, capped at MAX_CPUS */
    uint32_t cpu_apic_ids[MAX_CPUS];
    /* M103 */
    int ioapic_count;
    acpi_ioapic_t ioapics[MAX_IOAPICS];
    int override_count;
    acpi_irq_override_t overrides[MAX_IRQ_OVERRIDES];
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
 * M63 stretch goal: the `\_S5` sleep type lives in AML in the DSDT, and
 * for sixteen milestones this said "writing an AML parser is not in
 * scope" and power.c guessed. It reads it now - see acpi_find_s5, and
 * see its own comment for the precise, narrow sense in which that is a
 * parser at all. The guess remains as the fallback, because a machine
 * whose DSDT this cannot read is a machine that should still switch
 * off. */
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

/* M63 stretch goal: the S5 sleep-type values, read out of the DSDT's AML
 * rather than guessed.
 *
 * Returns 1 and fills *slp_a / *slp_b if the `\_S5_` package was found
 * and understood, 0 otherwise - and 0 is an ordinary answer that
 * power.c's own well-known-values fallback exists for.
 *
 * "An AML parser" overstates it and the overstatement matters: this
 * finds one named object by scanning for its encoding and decodes the
 * integer package that follows. It does not evaluate anything, has no
 * namespace, and would not survive a `_S5` defined inside a method or
 * behind an `If`. That covers every firmware this project has met and is
 * honestly less than the name suggests - which is why the fallback stays
 * and why the boot log says which one it used. */
int acpi_find_s5(uint8_t *slp_a, uint8_t *slp_b);
