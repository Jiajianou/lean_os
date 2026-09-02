#include "ioapic.h"

#include "acpi/acpi.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/pic.h"
#include "dev/fwcfg.h" /* M103 - which controller this boot was asked for */
#include "drivers/klog.h"
#include "lib/libk.h"
#include "mm/vmm.h"

/* The two memory-mapped registers every I/O APIC has: a selector and a
 * window. Everything else is reached by writing an index to the first
 * and reading or writing the second, which is why this driver is short
 * and why every access is a pair. */
#define IOAPIC_REGSEL 0x00
#define IOAPIC_IOWIN  0x10

#define IOAPIC_REG_ID     0x00
#define IOAPIC_REG_VER    0x01
#define IOAPIC_REG_REDTBL 0x10 /* entry N is registers 0x10+2N and 0x11+2N */

/* Redirection entry bits, low word. */
#define REDIR_MASKED        (1u << 16)
#define REDIR_LEVEL_TRIGGER (1u << 15)
#define REDIR_ACTIVE_LOW    (1u << 13)

static acpi_madt_info_t madt;
static int have_madt;
static int usable;

/* M103: per vector per CPU. 256 vectors is the whole space and MAX_CPUS
 * is 8, which is 16 KiB - the price of being able to tell an idle device
 * from a misrouted line. */
static uint64_t irq_counts[256][MAX_CPUS];

static volatile uint32_t *reg_window(const acpi_ioapic_t *io) {
    return (volatile uint32_t *)(uint64_t)io->address;
}

static uint32_t ioapic_read(const acpi_ioapic_t *io, uint32_t reg) {
    volatile uint32_t *base = reg_window(io);
    base[IOAPIC_REGSEL / 4] = reg;
    return base[IOAPIC_IOWIN / 4];
}

static void ioapic_write(const acpi_ioapic_t *io, uint32_t reg, uint32_t value) {
    volatile uint32_t *base = reg_window(io);
    base[IOAPIC_REGSEL / 4] = reg;
    base[IOAPIC_IOWIN / 4] = value;
}

/* How many redirection entries this one has, from its version register's
 * top byte. Asked rather than assumed: 24 is the usual answer and is not
 * a rule, and programming an entry a controller does not have writes
 * into whatever the window happens to alias. */
static uint32_t ioapic_entries(const acpi_ioapic_t *io) {
    return ((ioapic_read(io, IOAPIC_REG_VER) >> 16) & 0xFFu) + 1u;
}

/* The GSI an ISA IRQ is actually wired to, and the polarity/trigger the
 * firmware says it has. See the header: this indirection is the reason
 * the timer works. */
static uint32_t irq_to_gsi(uint8_t irq, uint16_t *out_flags) {
    *out_flags = 0;
    for (int i = 0; i < madt.override_count; i++) {
        if (madt.overrides[i].source == irq) {
            *out_flags = madt.overrides[i].flags;
            return madt.overrides[i].gsi;
        }
    }
    /* No override: an ISA IRQ is its own GSI, which is what the ACPI
     * spec says the absence of an entry means. */
    return irq;
}

static acpi_ioapic_t *ioapic_for_gsi(uint32_t gsi, uint32_t *out_index) {
    /* ---- the LAPIC has to be alive first ------------------------------
     *
     * An I/O APIC delivers to a local APIC, and a local APIC that has
     * not been software-enabled drops what it is sent. So a kernel that
     * routed IRQ 0 through the I/O APIC without enabling the LAPIC gets
     * no timer ticks at all - which is a hang at the first pit_sleep_ms
     * with nothing on the serial line to say why. That is exactly what
     * happened here, once.
     *
     * Enabling it twice is harmless (smp_init calls lapic_init again for
     * the BSP, and once per AP), and doing it here rather than making
     * boot order carry the dependency is the smaller thing: this file is
     * the one that knows it needs one. */
    lapic_init(madt.lapic_base);

    for (int i = 0; i < madt.ioapic_count; i++) {
        acpi_ioapic_t *io = &madt.ioapics[i];
        /* The controller's registers are memory-mapped at a physical
         * address well above the identity map's first gigabyte -
         * 0xFEC00000 on every machine anyone has - so the page has to be
         * mapped before the first read. Identity-mapped, like the LAPIC
         * beside it (lapic.c does the same thing at 0xFEE00000 and for
         * the same reason), so the physical address in the MADT is also
         * the address this code dereferences. */
        vmm_map_page((uint64_t)io->address & ~0xFFFull,
                     (uint64_t)io->address & ~0xFFFull, VMM_FLAG_WRITABLE);
        uint32_t count = ioapic_entries(io);
        if (gsi >= io->gsi_base && gsi < io->gsi_base + count) {
            *out_index = gsi - io->gsi_base;
            return io;
        }
    }
    return 0;
}

void ioapic_init(void) {
    /* Asked before anything is read: see kernel/dev/fwcfg.h for the
     * measurement behind the default and for why the switch comes from
     * outside the image rather than from a build flag. */
    if (!boot_ioapic_enabled()) {
        klog_puts("[ioapic] not requested for this boot - on the 8259 PIC. "
                   "-fw_cfg name=opt/leanos/ioapic,string=1 selects it; "
                   "kernel/dev/fwcfg.h has the numbers.\n");
        usable = 0;
        return;
    }
    have_madt = acpi_find_madt(&madt);
    if (!have_madt || madt.ioapic_count == 0) {
        klog_puts("[ioapic] no I/O APIC in the MADT - staying on the 8259 PIC.\n");
        usable = 0;
        return;
    }
    /* ---- the LAPIC has to be alive first ------------------------------
     *
     * An I/O APIC delivers to a local APIC, and a local APIC that has
     * not been software-enabled drops what it is sent. So a kernel that
     * routed IRQ 0 through the I/O APIC without enabling the LAPIC gets
     * no timer ticks at all - which is a hang at the first pit_sleep_ms
     * with nothing on the serial line to say why. That is exactly what
     * happened here, once.
     *
     * Enabling it twice is harmless (smp_init calls lapic_init again for
     * the BSP, and once per AP), and doing it here rather than making
     * boot order carry the dependency is the smaller thing: this file is
     * the one that knows it needs one. */
    lapic_init(madt.lapic_base);

    for (int i = 0; i < madt.ioapic_count; i++) {
        acpi_ioapic_t *io = &madt.ioapics[i];
        /* The controller's registers are memory-mapped at a physical
         * address well above the identity map's first gigabyte -
         * 0xFEC00000 on every machine anyone has - so the page has to be
         * mapped before the first read. Identity-mapped, like the LAPIC
         * beside it (lapic.c does the same thing at 0xFEE00000 and for
         * the same reason), so the physical address in the MADT is also
         * the address this code dereferences. */
        vmm_map_page((uint64_t)io->address & ~0xFFFull,
                     (uint64_t)io->address & ~0xFFFull, VMM_FLAG_WRITABLE);
        uint32_t count = ioapic_entries(io);
        /* Every entry masked, before anything routes one. A redirection
         * table comes out of firmware in an unspecified state, and an
         * unmasked entry pointing at a vector this kernel has not set up
         * is an interrupt storm at the first device that twitches. */
        for (uint32_t e = 0; e < count; e++) {
            ioapic_write(io, IOAPIC_REG_REDTBL + 2 * e, REDIR_MASKED);
            ioapic_write(io, IOAPIC_REG_REDTBL + 2 * e + 1, 0);
        }
        klog_puts("[ioapic] id=0x");
        klog_put_hex32(io->id);
        klog_puts(" at 0x");
        klog_put_hex32(io->address);
        klog_puts(" gsi_base=0x");
        klog_put_hex32(io->gsi_base);
        klog_puts(" entries=0x");
        klog_put_hex32(count);
        klog_putc('\n');
    }
    usable = 1;
}

int ioapic_available(void) {
    return usable;
}

void ioapic_route_irq(uint8_t irq, uint8_t lapic_id) {
    if (!usable) {
        pic_clear_mask(irq); /* no I/O APIC: the PIC is the controller */
        return;
    }
    uint16_t flags = 0;
    uint32_t gsi = irq_to_gsi(irq, &flags);
    uint32_t index = 0;
    acpi_ioapic_t *io = ioapic_for_gsi(gsi, &index);
    if (!io) {
        klog_puts("[ioapic] no controller serves gsi 0x");
        klog_put_hex32(gsi);
        klog_puts(" - falling back to the PIC for this line\n");
        pic_clear_mask(irq);
        return;
    }

    uint32_t low = (uint32_t)(PIC_IRQ_VECTOR_OFFSET + irq);
    /* The MADT's flag encoding: bits 0-1 polarity, bits 2-3 trigger.
     * 0 in either means "conforms to the bus", and for an ISA line that
     * is edge-triggered and active high - which is why the default here
     * is neither bit set rather than a guess. */
    if ((flags & 0x3) == 3) {
        low |= REDIR_ACTIVE_LOW;
    }
    if ((flags & 0xC) == 0xC) {
        low |= REDIR_LEVEL_TRIGGER;
    }
    /* The destination goes in the HIGH word, in bits 24-31, and the
     * entry is written high-word-first with the low word still masked -
     * so there is no instant in which a live entry points at a
     * destination that has not been set. */
    ioapic_write(io, IOAPIC_REG_REDTBL + 2 * index, low | REDIR_MASKED);
    ioapic_write(io, IOAPIC_REG_REDTBL + 2 * index + 1, (uint32_t)lapic_id << 24);
    ioapic_write(io, IOAPIC_REG_REDTBL + 2 * index, low);
}

void ioapic_mask_irq(uint8_t irq) {
    if (!usable) {
        pic_set_mask(irq);
        return;
    }
    uint16_t flags = 0;
    uint32_t gsi = irq_to_gsi(irq, &flags);
    uint32_t index = 0;
    acpi_ioapic_t *io = ioapic_for_gsi(gsi, &index);
    if (!io) {
        return;
    }
    uint32_t low = ioapic_read(io, IOAPIC_REG_REDTBL + 2 * index);
    ioapic_write(io, IOAPIC_REG_REDTBL + 2 * index, low | REDIR_MASKED);
}

void ioapic_count_irq(uint8_t vector, int cpu) {
    if (cpu >= 0 && cpu < MAX_CPUS) {
        /* No lock and no atomic: each CPU writes only its own column, and
         * a reader that catches a torn 64-bit read gets a count off by
         * one out of thousands. The same argument sched.c's per-CPU tick
         * counters make. */
        irq_counts[vector][cpu]++;
    }
}

uint64_t ioapic_irq_count(uint8_t vector, int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? irq_counts[vector][cpu] : 0;
}
