#include "acpi.h"

#include "drivers/kernel_log.h"
#include "memory_management/virtual_memory.h"
#include "panic.h"

typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

static int acpi_address_readable(uint64_t phys, uint64_t length) {
    return phys != 0 && virtual_memory_identity_covers(phys, length);
}

static int sig_eq(const void *a, const char *b, int length) {
    const uint8_t *pa = (const uint8_t *)a;
    for (int i = 0; i < length; i++) {
        if (pa[i] != (uint8_t)b[i]) {
            return 0;
        }
    }
    return 1;
}

static uint64_t handoff_rsdp_phys;

void acpi_set_rsdp(uint64_t phys) {
    handoff_rsdp_phys = phys;
}

static const acpi_rsdp_t *find_rsdp(void) {
    if (acpi_address_readable(handoff_rsdp_phys, sizeof(acpi_rsdp_t)) &&
        sig_eq((const void *)(uintptr_t)handoff_rsdp_phys, "RSD PTR ", 8)) {
        return (const acpi_rsdp_t *)(uintptr_t)handoff_rsdp_phys;
    }

    uint16_t ebda_seg = *(const uint16_t *)(uintptr_t)0x40EUL;
    uint64_t ebda_address = (uint64_t)ebda_seg << 4;
    if (ebda_address != 0) {
        for (uint64_t address = ebda_address; address < ebda_address + 1024; address += 16) {
            if (sig_eq((const void *)(uintptr_t)address, "RSD PTR ", 8)) {
                return (const acpi_rsdp_t *)(uintptr_t)address;
            }
        }
    }
    for (uint64_t address = 0xE0000UL; address < 0x100000UL; address += 16) {
        if (sig_eq((const void *)(uintptr_t)address, "RSD PTR ", 8)) {
            return (const acpi_rsdp_t *)(uintptr_t)address;
        }
    }
    return (const acpi_rsdp_t *)0;
}

static const acpi_sdt_header_t *table_at(uint64_t phys) {
    if (!acpi_address_readable(phys, sizeof(acpi_sdt_header_t))) {
        return (const acpi_sdt_header_t *)0;
    }
    return (const acpi_sdt_header_t *)(uintptr_t)phys;
}

static const acpi_sdt_header_t *find_table(const char *sig) {
    const acpi_rsdp_t *rsdp = find_rsdp();
    if (!rsdp) {
        return (const acpi_sdt_header_t *)0;
    }
    int use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const acpi_sdt_header_t *root = table_at(use_xsdt ? rsdp->xsdt_address : (uint64_t)rsdp->rsdt_address);
    if (!root || !sig_eq(root->signature, use_xsdt ? "XSDT" : "RSDT", 4)) {
        return (const acpi_sdt_header_t *)0;
    }
    int entry_size = use_xsdt ? 8 : 4;
    int entry_count = (int)((root->length - (uint32_t)sizeof(acpi_sdt_header_t)) / (uint32_t)entry_size);
    const uint8_t *entries = (const uint8_t *)root + sizeof(acpi_sdt_header_t);
    for (int i = 0; i < entry_count; i++) {
        uint64_t table_phys = use_xsdt ? *(const uint64_t *)(entries + i * 8)
                                        : (uint64_t) * (const uint32_t *)(entries + i * 4);
        const acpi_sdt_header_t *header = table_at(table_phys);
        if (header && sig_eq(header->signature, sig, 4)) {
            return header;
        }
    }
    return (const acpi_sdt_header_t *)0;
}

#define FADT_SMI_COMMAND      48
#define FADT_ACPI_ENABLE  52
#define FADT_PM1A_COUNT_BLOCK_DEVICE 64
#define FADT_PM1B_COUNT_BLOCK_DEVICE 68
#define FADT_FLAGS        112
#define FADT_RESET_REG    116
#define FADT_RESET_VALUE  128
#define FADT_FLAG_RESET_REG_SUP (1u << 10)
#define GAS_SPACE_SYSTEM_IO 1

int acpi_find_power(acpi_power_info_t *out) {
    const acpi_sdt_header_t *fadt = find_table("FACP");
    if (!fadt) {
        kernel_log_puts("[acpi] no FADT found - power off/reset will use their fallback tiers.\n");
        return 0;
    }
    const uint8_t *t = (const uint8_t *)fadt;
    out->pm1a_count = 0;
    out->pm1b_count = 0;
    out->smi_command = 0;
    out->acpi_enable = 0;
    out->reset_port = 0;
    out->reset_value = 0;

    if (fadt->length > FADT_PM1A_COUNT_BLOCK_DEVICE + 4) {
        out->pm1a_count = *(const uint32_t *)(t + FADT_PM1A_COUNT_BLOCK_DEVICE);
    }
    if (fadt->length > FADT_PM1B_COUNT_BLOCK_DEVICE + 4) {
        out->pm1b_count = *(const uint32_t *)(t + FADT_PM1B_COUNT_BLOCK_DEVICE);
    }
    if (fadt->length > FADT_ACPI_ENABLE) {
        out->smi_command = *(const uint32_t *)(t + FADT_SMI_COMMAND);
        out->acpi_enable = t[FADT_ACPI_ENABLE];
    }
    if (fadt->length > FADT_RESET_VALUE) {
        uint32_t flags = *(const uint32_t *)(t + FADT_FLAGS);
        const uint8_t *gas = t + FADT_RESET_REG;
        uint64_t address = *(const uint64_t *)(gas + 4);
        if ((flags & FADT_FLAG_RESET_REG_SUP) && gas[0] == GAS_SPACE_SYSTEM_IO &&
            address != 0 && address <= 0xFFFF) {
            out->reset_port = (uint32_t)address;
            out->reset_value = t[FADT_RESET_VALUE];
        }
    }

    kernel_log_puts("[acpi] FADT found: PM1a_CNT=0x");
    kernel_log_put_hex32(out->pm1a_count);
    kernel_log_puts(", PM1b_CNT=0x");
    kernel_log_put_hex32(out->pm1b_count);
    kernel_log_puts(", reset port=0x");
    kernel_log_put_hex32(out->reset_port);
    kernel_log_puts(".\n");
    return 1;
}

#define AML_NAME_OP    0x08
#define AML_PACKAGE_OP 0x12
#define AML_ZERO_OP    0x00
#define AML_ONE_OP     0x01
#define AML_BYTE_PREFIX 0x0A
#define AML_WORD_PREFIX 0x0B
#define AML_ROOT_CHAR  0x5C

#define FADT_DSDT   40
#define FADT_X_DSDT 140

static int aml_read_int(const uint8_t *p, uint32_t length, uint32_t *at, uint8_t *out) {
    if (*at >= length) {
        return 0;
    }
    uint8_t op = p[(*at)++];
    if (op == AML_ZERO_OP) {
        *out = 0;
        return 1;
    }
    if (op == AML_ONE_OP) {
        *out = 1;
        return 1;
    }
    if (op == AML_BYTE_PREFIX && *at < length) {
        *out = p[(*at)++];
        return 1;
    }
    if (op == AML_WORD_PREFIX && *at + 1 < length) {
        *out = p[*at];
        *at += 2;
        return 1;
    }
    return 0;
}

int acpi_find_s5(uint8_t *slp_a, uint8_t *slp_b) {
    const acpi_sdt_header_t *fadt = find_table("FACP");
    if (!fadt) {
        return 0;
    }
    const uint8_t *f = (const uint8_t *)fadt;
    uint64_t dsdt_phys = 0;
    if (fadt->length > FADT_X_DSDT + 8) {
        dsdt_phys = *(const uint64_t *)(f + FADT_X_DSDT);
    }
    if (dsdt_phys == 0 && fadt->length > FADT_DSDT + 4) {
        dsdt_phys = (uint64_t)*(const uint32_t *)(f + FADT_DSDT);
    }
    const acpi_sdt_header_t *dsdt = table_at(dsdt_phys);
    if (!dsdt || !sig_eq(dsdt->signature, "DSDT", 4) ||
        dsdt->length <= sizeof(acpi_sdt_header_t)) {
        return 0;
    }

    const uint8_t *aml = (const uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
    uint32_t length = dsdt->length - (uint32_t)sizeof(acpi_sdt_header_t);

    for (uint32_t i = 0; i + 6 < length; i++) {
        if (aml[i] != AML_NAME_OP) {
            continue;
        }
        uint32_t n = i + 1;
        if (aml[n] == AML_ROOT_CHAR) {
            n++;
        }
        if (n + 4 > length || aml[n] != '_' || aml[n + 1] != 'S' ||
            aml[n + 2] != '5' || aml[n + 3] != '_') {
            continue;
        }
        n += 4;
        if (n >= length || aml[n++] != AML_PACKAGE_OP) {
            continue;
        }
        if (n >= length) {
            continue;
        }
        uint32_t extra = (uint32_t)(aml[n] >> 6);
        n += 1 + extra;
        if (n >= length) {
            continue;
        }
        uint8_t count = aml[n++];
        if (count < 1) {
            continue;
        }
        uint8_t a = 0, b = 0;
        if (!aml_read_int(aml, length, &n, &a)) {
            continue;
        }
        if (count < 2 || !aml_read_int(aml, length, &n, &b)) {
            b = a;
        }
        *slp_a = (uint8_t)(a & 0x07);
        *slp_b = (uint8_t)(b & 0x07);
        kernel_log_puts("[acpi] \\_S5 read from the DSDT: SLP_TYPa=0x");
        kernel_log_put_hex32(*slp_a);
        kernel_log_puts(", SLP_TYPb=0x");
        kernel_log_put_hex32(*slp_b);
        kernel_log_puts(" - no longer a guess.\n");
        return 1;
    }
    kernel_log_puts("[acpi] no readable \\_S5 in the DSDT - power off will try the well-known values.\n");
    return 0;
}

int acpi_find_madt(acpi_madt_info_t *out) {
    const acpi_sdt_header_t *madt = find_table("APIC");
    if (!madt) {
        kernel_log_puts("[acpi] no MADT (APIC table) found - continuing single-core.\n");
        return 0;
    }

    const uint8_t *madt_body = (const uint8_t *)madt + sizeof(acpi_sdt_header_t);
    out->lapic_base = *(const uint32_t *)madt_body;
    out->cpu_count = 0;
    out->ioapic_count = 0;
    out->override_count = 0;

    const uint8_t *p = madt_body + 8;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t length = p[1];
        if (length < 2 || p + length > end) {
            break;
        }
        if (type == 0 && length >= 8) {
            uint8_t apic_id = p[3];
            uint32_t flags = *(const uint32_t *)(p + 4);
            if ((flags & 1) && out->cpu_count < MAX_CPUS) {
                out->cpu_apic_ids[out->cpu_count++] = apic_id;
            }
        } else if (type == 1 && length >= 12) {
            if (out->ioapic_count < MAX_IOAPICS) {
                acpi_ioapic_t *io = &out->ioapics[out->ioapic_count++];
                io->id = p[2];
                io->address = *(const uint32_t *)(p + 4);
                io->gsi_base = *(const uint32_t *)(p + 8);
            }
        } else if (type == 2 && length >= 10) {
            if (out->override_count < MAX_IRQ_OVERRIDES) {
                acpi_irq_override_t *ov = &out->overrides[out->override_count++];
                ov->source = p[3];
                ov->gsi = *(const uint32_t *)(p + 4);
                ov->flags = *(const uint16_t *)(p + 8);
            }
        } else if (type == 5 && length >= 12) {
            out->lapic_base = *(const uint64_t *)(p + 4);
        }
        p += length;
    }

    kernel_log_puts("[acpi] MADT found: lapic_base=0x");
    kernel_log_put_hex64(out->lapic_base);
    kernel_log_puts(", ");
    kernel_log_put_hex32((uint32_t)out->cpu_count);
    kernel_log_puts(" enabled CPU(s), ");
    kernel_log_put_hex32((uint32_t)out->ioapic_count);
    kernel_log_puts(" I/O APIC(s), ");
    kernel_log_put_hex32((uint32_t)out->override_count);
    kernel_log_puts(" interrupt source override(s).\n");
    return 1;
}
