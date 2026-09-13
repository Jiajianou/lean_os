#include "ioapic.h"

#include "acpi/acpi.h"
#include "architecture/x86_64/cpu.h"
#include "architecture/x86_64/lapic.h"
#include "architecture/x86_64/pic.h"
#include "device/fwcfg.h"
#include "drivers/kernel_log.h"
#include "library/kernel_library.h"
#include "memory_management/virtual_memory.h"

#define IOAPIC_REGSEL 0x00
#define IOAPIC_IOWIN  0x10

#define IOAPIC_REG_ID     0x00
#define IOAPIC_REG_VER    0x01
#define IOAPIC_REG_REDTBL 0x10

#define REDIR_MASKED        (1u << 16)
#define REDIR_LEVEL_TRIGGER (1u << 15)
#define REDIR_ACTIVE_LOW    (1u << 13)

static acpi_madt_info_t madt;
static int have_madt;
static int usable;

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

static uint32_t ioapic_entries(const acpi_ioapic_t *io) {
    return ((ioapic_read(io, IOAPIC_REG_VER) >> 16) & 0xFFu) + 1u;
}

static uint32_t irq_to_gsi(uint8_t irq, uint16_t *out_flags) {
    *out_flags = 0;
    for (int i = 0; i < madt.override_count; i++) {
        if (madt.overrides[i].source == irq) {
            *out_flags = madt.overrides[i].flags;
            return madt.overrides[i].gsi;
        }
    }
    return irq;
}

static acpi_ioapic_t *ioapic_for_gsi(uint32_t gsi, uint32_t *out_index) {
    lapic_init(madt.lapic_base);

    for (int i = 0; i < madt.ioapic_count; i++) {
        acpi_ioapic_t *io = &madt.ioapics[i];
        virtual_memory_map_page((uint64_t)io->address & ~0xFFFull,
                     (uint64_t)io->address & ~0xFFFull, VIRTUAL_MEMORY_FLAG_WRITABLE);
        uint32_t count = ioapic_entries(io);
        if (gsi >= io->gsi_base && gsi < io->gsi_base + count) {
            *out_index = gsi - io->gsi_base;
            return io;
        }
    }
    return 0;
}

void ioapic_init(void) {
    if (!boot_ioapic_enabled()) {
        kernel_log_puts("[ioapic] not requested for this boot - on the 8259 PIC. "
                   "-fw_cfg name=opt/leanos/ioapic,string=1 selects it; "
                   "kernel/device/fwcfg.h has the numbers.\n");
        usable = 0;
        return;
    }
    have_madt = acpi_find_madt(&madt);
    if (!have_madt || madt.ioapic_count == 0) {
        kernel_log_puts("[ioapic] no I/O APIC in the MADT - staying on the 8259 PIC.\n");
        usable = 0;
        return;
    }
    lapic_init(madt.lapic_base);

    for (int i = 0; i < madt.ioapic_count; i++) {
        acpi_ioapic_t *io = &madt.ioapics[i];
        virtual_memory_map_page((uint64_t)io->address & ~0xFFFull,
                     (uint64_t)io->address & ~0xFFFull, VIRTUAL_MEMORY_FLAG_WRITABLE);
        uint32_t count = ioapic_entries(io);
        for (uint32_t e = 0; e < count; e++) {
            ioapic_write(io, IOAPIC_REG_REDTBL + 2 * e, REDIR_MASKED);
            ioapic_write(io, IOAPIC_REG_REDTBL + 2 * e + 1, 0);
        }
        kernel_log_puts("[ioapic] id=0x");
        kernel_log_put_hex32(io->id);
        kernel_log_puts(" at 0x");
        kernel_log_put_hex32(io->address);
        kernel_log_puts(" gsi_base=0x");
        kernel_log_put_hex32(io->gsi_base);
        kernel_log_puts(" entries=0x");
        kernel_log_put_hex32(count);
        kernel_log_putc('\n');
    }
    usable = 1;
}

int ioapic_available(void) {
    return usable;
}

void ioapic_route_irq(uint8_t irq, uint8_t lapic_id) {
    if (!usable) {
        pic_clear_mask(irq);
        return;
    }
    uint16_t flags = 0;
    uint32_t gsi = irq_to_gsi(irq, &flags);
    uint32_t index = 0;
    acpi_ioapic_t *io = ioapic_for_gsi(gsi, &index);
    if (!io) {
        kernel_log_puts("[ioapic] no controller serves gsi 0x");
        kernel_log_put_hex32(gsi);
        kernel_log_puts(" - falling back to the PIC for this line\n");
        pic_clear_mask(irq);
        return;
    }

    uint32_t low = (uint32_t)(PIC_IRQ_VECTOR_OFFSET + irq);
    if ((flags & 0x3) == 3) {
        low |= REDIR_ACTIVE_LOW;
    }
    if ((flags & 0xC) == 0xC) {
        low |= REDIR_LEVEL_TRIGGER;
    }
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
        irq_counts[vector][cpu]++;
    }
}

uint64_t ioapic_irq_count(uint8_t vector, int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? irq_counts[vector][cpu] : 0;
}
