#include "acpi.h"

#include "drivers/klog.h"
#include "panic.h"

typedef struct __attribute__((packed)) {
    char signature[8]; /* "RSD PTR " */
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision; /* 0 = ACPI 1.0 (RSDT only), >=2 = ACPI 2.0+ (XSDT available) */
    uint32_t rsdt_address;
    /* ACPI 2.0+ only - only valid if revision >= 2, not read otherwise */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length; /* whole table, header included */
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

/* Every physical address this file dereferences - the RSDP search range,
 * and every table the RSDT/XSDT points at - is expected to fall inside the
 * 1 GiB vmm.c always identity-maps (the same assumption pmm/vmm already
 * document). True on QEMU's default machine, but M29: NOT something a
 * real machine's firmware owes this kernel - ACPI tables commonly live in
 * high reserved memory well above 1 GiB on real hardware, and this was
 * the one ACPI outcome that still panicked instead of degrading, exactly
 * the "assumes present, panics if not" pattern M27 already flagged and
 * fixed for RTL8139/PS2 - see table_at, below, which is where this
 * actually got softened once M28's real-hardware boot made it a real
 * risk rather than a hypothetical one. */
#define ACPI_IDENTITY_LIMIT 0x40000000ULL

static int sig_eq(const void *a, const char *b, int len) {
    const uint8_t *pa = (const uint8_t *)a;
    for (int i = 0; i < len; i++) {
        if (pa[i] != (uint8_t)b[i]) {
            return 0;
        }
    }
    return 1;
}

static const acpi_rsdp_t *find_rsdp(void) {
    /* EBDA base address is a segment stored at the fixed BIOS Data Area
     * offset 0x40E; a physical address of 0 means "no EBDA reported",
     * which some BIOSes (QEMU included, depending on version) do - skip
     * straight to the fixed BIOS ROM range in that case rather than
     * scanning from a bogus base. */
    uint16_t ebda_seg = *(const uint16_t *)(uintptr_t)0x40EUL;
    uint64_t ebda_addr = (uint64_t)ebda_seg << 4;
    if (ebda_addr != 0) {
        for (uint64_t addr = ebda_addr; addr < ebda_addr + 1024; addr += 16) {
            if (sig_eq((const void *)(uintptr_t)addr, "RSD PTR ", 8)) {
                return (const acpi_rsdp_t *)(uintptr_t)addr;
            }
        }
    }
    for (uint64_t addr = 0xE0000UL; addr < 0x100000UL; addr += 16) {
        if (sig_eq((const void *)(uintptr_t)addr, "RSD PTR ", 8)) {
            return (const acpi_rsdp_t *)(uintptr_t)addr;
        }
    }
    return (const acpi_rsdp_t *)0;
}

/* M29: returns NULL (not a panic) for a table address outside the
 * identity-mapped range - real firmware placing ACPI tables in high
 * reserved memory is a legitimate, expected outcome on real hardware,
 * not kernel/hardware misbehavior. Every caller already has a
 * "couldn't find what I needed, fall back to single-core" path for
 * every other ACPI-absent case (find_rsdp returning NULL, a signature
 * mismatch, no MADT) - this makes an out-of-range table pointer just
 * another instance of that same path instead of the one outcome that
 * used to take the whole kernel down. */
static const acpi_sdt_header_t *table_at(uint64_t phys) {
    if (phys == 0 || phys >= ACPI_IDENTITY_LIMIT) {
        return (const acpi_sdt_header_t *)0;
    }
    return (const acpi_sdt_header_t *)(uintptr_t)phys;
}

int acpi_find_madt(acpi_madt_info_t *out) {
    const acpi_rsdp_t *rsdp = find_rsdp();
    if (!rsdp) {
        klog_puts("[acpi] no RSDP found - platform may not support ACPI; continuing single-core.\n");
        return 0;
    }

    int use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    const acpi_sdt_header_t *root = table_at(use_xsdt ? rsdp->xsdt_address : (uint64_t)rsdp->rsdt_address);
    if (!root) {
        klog_puts("[acpi] root table (RSDT/XSDT) outside the identity-mapped range - continuing single-core.\n");
        return 0;
    }
    if (!sig_eq(root->signature, use_xsdt ? "XSDT" : "RSDT", 4)) {
        klog_puts("[acpi] root table signature mismatch - continuing single-core.\n");
        return 0;
    }

    int entry_size = use_xsdt ? 8 : 4;
    int entry_count = (int)((root->length - (uint32_t)sizeof(acpi_sdt_header_t)) / (uint32_t)entry_size);
    const uint8_t *entries = (const uint8_t *)root + sizeof(acpi_sdt_header_t);

    const acpi_sdt_header_t *madt = (const acpi_sdt_header_t *)0;
    for (int i = 0; i < entry_count; i++) {
        uint64_t table_phys = use_xsdt ? *(const uint64_t *)(entries + i * 8)
                                        : (uint64_t) * (const uint32_t *)(entries + i * 4);
        const acpi_sdt_header_t *hdr = table_at(table_phys);
        if (hdr && sig_eq(hdr->signature, "APIC", 4)) {
            madt = hdr;
            break;
        }
    }
    if (!madt) {
        klog_puts("[acpi] no MADT (APIC table) found - continuing single-core.\n");
        return 0;
    }

    /* MADT body, right after the standard SDT header: local_apic_address
     * (4 bytes), flags (4 bytes), then a stream of variable-length
     * entries (Intel ACPI spec 5.2.12). Only types this kernel acts on:
     * type 0 (Processor Local APIC) and type 5 (Local APIC Address
     * Override) - everything else (I/O APIC, interrupt source overrides,
     * x2APIC entries for >255 CPUs, ...) is out of scope for the minimal
     * "start every enabled core" job this does. */
    const uint8_t *madt_body = (const uint8_t *)madt + sizeof(acpi_sdt_header_t);
    out->lapic_base = *(const uint32_t *)madt_body;
    out->cpu_count = 0;

    const uint8_t *p = madt_body + 8;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len = p[1];
        if (len < 2 || p + len > end) {
            break; /* malformed entry - stop rather than walk off the table */
        }
        if (type == 0 && len >= 8) { /* Processor Local APIC */
            uint8_t apic_id = p[3];
            uint32_t flags = *(const uint32_t *)(p + 4);
            if ((flags & 1) && out->cpu_count < MAX_CPUS) {
                out->cpu_apic_ids[out->cpu_count++] = apic_id;
            }
        } else if (type == 5 && len >= 12) { /* Local APIC Address Override */
            out->lapic_base = *(const uint64_t *)(p + 4);
        }
        p += len;
    }

    klog_puts("[acpi] MADT found: lapic_base=0x");
    klog_put_hex64(out->lapic_base);
    klog_puts(", ");
    klog_put_hex32((uint32_t)out->cpu_count);
    klog_puts(" enabled CPU(s) listed.\n");
    return 1;
}
