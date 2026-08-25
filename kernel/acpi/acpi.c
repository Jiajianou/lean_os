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

/* Set once by kernel_main from the boot loader's handoff - see
 * acpi_set_rsdp. 0 means "the firmware told us nothing", which sends
 * find_rsdp back to its legacy scan. */
static uint64_t handoff_rsdp_phys;

void acpi_set_rsdp(uint64_t phys) {
    handoff_rsdp_phys = phys;
}

static const acpi_rsdp_t *find_rsdp(void) {
    /* M47: the firmware's own answer first. Still signature-checked
     * rather than trusted: a pointer that doesn't start with "RSD PTR "
     * is not an RSDP whatever handed it over, and falling through to the
     * scan is a better outcome than parsing whatever is there. */
    if (handoff_rsdp_phys != 0 && handoff_rsdp_phys < ACPI_IDENTITY_LIMIT &&
        sig_eq((const void *)(uintptr_t)handoff_rsdp_phys, "RSD PTR ", 8)) {
        return (const acpi_rsdp_t *)(uintptr_t)handoff_rsdp_phys;
    }

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

/* M47: the RSDT/XSDT walk, lifted out of acpi_find_madt now that a second
 * caller wants a different table out of the same list. Returns the table
 * whose signature is `sig`, or NULL - and NULL genuinely covers every
 * "this platform didn't give us one" case (no RSDP, a root table outside
 * the identity map, a signature mismatch, no such table), which is
 * exactly what both callers already treat as a normal fallback rather
 * than an error. Silent by design: the two callers say different things
 * about a missing table, so the message belongs to them. */
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
        const acpi_sdt_header_t *hdr = table_at(table_phys);
        if (hdr && sig_eq(hdr->signature, sig, 4)) {
            return hdr;
        }
    }
    return (const acpi_sdt_header_t *)0;
}

/* FADT field offsets from the start of the table (ACPI spec 5.2.9's
 * "Fixed ACPI Description Table" layout). Written out as offsets rather
 * than as a packed struct because only six of forty-odd fields are ever
 * read here, and a struct would have to be correct about all of them. */
#define FADT_SMI_CMD      48
#define FADT_ACPI_ENABLE  52
#define FADT_PM1A_CNT_BLK 64
#define FADT_PM1B_CNT_BLK 68
#define FADT_FLAGS        112
#define FADT_RESET_REG    116 /* a 12-byte Generic Address Structure */
#define FADT_RESET_VALUE  128
#define FADT_FLAG_RESET_REG_SUP (1u << 10)
#define GAS_SPACE_SYSTEM_IO 1

int acpi_find_power(acpi_power_info_t *out) {
    const acpi_sdt_header_t *fadt = find_table("FACP"); /* the FADT's signature is "FACP", not "FADT" */
    if (!fadt) {
        klog_puts("[acpi] no FADT found - power off/reset will use their fallback tiers.\n");
        return 0;
    }
    const uint8_t *t = (const uint8_t *)fadt;
    out->pm1a_cnt = 0;
    out->pm1b_cnt = 0;
    out->smi_cmd = 0;
    out->acpi_enable = 0;
    out->reset_port = 0;
    out->reset_value = 0;

    /* Every read is bounds-checked against the table's own declared
     * length: RESET_REG in particular only exists on ACPI 2.0+ FADTs, and
     * a 1.0 one is genuinely shorter than the offset it would live at. */
    if (fadt->length > FADT_PM1A_CNT_BLK + 4) {
        out->pm1a_cnt = *(const uint32_t *)(t + FADT_PM1A_CNT_BLK);
    }
    if (fadt->length > FADT_PM1B_CNT_BLK + 4) {
        out->pm1b_cnt = *(const uint32_t *)(t + FADT_PM1B_CNT_BLK);
    }
    if (fadt->length > FADT_ACPI_ENABLE) {
        out->smi_cmd = *(const uint32_t *)(t + FADT_SMI_CMD);
        out->acpi_enable = t[FADT_ACPI_ENABLE];
    }
    if (fadt->length > FADT_RESET_VALUE) {
        uint32_t flags = *(const uint32_t *)(t + FADT_FLAGS);
        const uint8_t *gas = t + FADT_RESET_REG;
        uint64_t addr = *(const uint64_t *)(gas + 4);
        /* Only a SystemIO reset register is usable here: a memory-mapped
         * one would need a vmm mapping this kernel has no reason to make
         * for a single byte, and the 8042 tier below is a perfectly good
         * answer when it isn't. */
        if ((flags & FADT_FLAG_RESET_REG_SUP) && gas[0] == GAS_SPACE_SYSTEM_IO &&
            addr != 0 && addr <= 0xFFFF) {
            out->reset_port = (uint32_t)addr;
            out->reset_value = t[FADT_RESET_VALUE];
        }
    }

    klog_puts("[acpi] FADT found: PM1a_CNT=0x");
    klog_put_hex32(out->pm1a_cnt);
    klog_puts(", PM1b_CNT=0x");
    klog_put_hex32(out->pm1b_cnt);
    klog_puts(", reset port=0x");
    klog_put_hex32(out->reset_port);
    klog_puts(".\n");
    return 1;
}

/* ---- M63 stretch goal: reading \_S5 out of the DSDT --------------------
 *
 * AML is a bytecode with a namespace, methods, and control flow, and a
 * real interpreter for it is a subsystem. This is not that, and the
 * header says so: it looks for one specific encoding and decodes the
 * package that follows it.
 *
 * What a `\_S5` definition compiles to is:
 *
 *     08              NameOp
 *     5C 5F 53 35 5F  the name, "\_S5_" (the leading 5C - a RootChar -
 *                     is optional; both spellings appear in the wild)
 *     12              PackageOp
 *     <pkglen>        1-4 bytes, the top two bits of the first saying how
 *                     many more follow
 *     <count>         number of elements
 *     <elem> ...      SLP_TYPa, SLP_TYPb, and two the OS does not use
 *
 * Each element is a small integer: ZeroOp, OneOp, or a BytePrefix and a
 * byte. Anything else here is a definition this cannot read, which is
 * reported as "not found" rather than guessed at - a wrong SLP_TYP is a
 * machine that does not switch off, and the fallback is a better guess
 * than a misparse.
 */
#define AML_NAME_OP    0x08
#define AML_PACKAGE_OP 0x12
#define AML_ZERO_OP    0x00
#define AML_ONE_OP     0x01
#define AML_BYTE_PREFIX 0x0A
#define AML_WORD_PREFIX 0x0B
#define AML_ROOT_CHAR  0x5C

#define FADT_DSDT   40  /* 32-bit physical address of the DSDT */
#define FADT_X_DSDT 140 /* 64-bit, ACPI 2.0+ - preferred when the table is long enough to have one */

/* One package element as an integer, advancing *at. Returns 0 if the
 * element is not one of the small-integer encodings this understands. */
static int aml_read_int(const uint8_t *p, uint32_t len, uint32_t *at, uint8_t *out) {
    if (*at >= len) {
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
    if (op == AML_BYTE_PREFIX && *at < len) {
        *out = p[(*at)++];
        return 1;
    }
    if (op == AML_WORD_PREFIX && *at + 1 < len) {
        *out = p[*at]; /* SLP_TYP is three bits; the high byte cannot matter */
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
    uint32_t len = dsdt->length - (uint32_t)sizeof(acpi_sdt_header_t);

    for (uint32_t i = 0; i + 6 < len; i++) {
        if (aml[i] != AML_NAME_OP) {
            continue;
        }
        uint32_t n = i + 1;
        if (aml[n] == AML_ROOT_CHAR) {
            n++;
        }
        if (n + 4 > len || aml[n] != '_' || aml[n + 1] != 'S' ||
            aml[n + 2] != '5' || aml[n + 3] != '_') {
            continue;
        }
        n += 4;
        if (n >= len || aml[n++] != AML_PACKAGE_OP) {
            continue;
        }
        /* PkgLength: the top two bits of the lead byte say how many more
         * bytes it occupies. The value itself is not needed - the element
         * count that follows bounds the read - but the bytes have to be
         * stepped over. */
        if (n >= len) {
            continue;
        }
        uint32_t extra = (uint32_t)(aml[n] >> 6);
        n += 1 + extra;
        if (n >= len) {
            continue;
        }
        uint8_t count = aml[n++];
        if (count < 1) {
            continue;
        }
        uint8_t a = 0, b = 0;
        if (!aml_read_int(aml, len, &n, &a)) {
            continue;
        }
        if (count < 2 || !aml_read_int(aml, len, &n, &b)) {
            b = a; /* a single-element package means both registers take the same value */
        }
        *slp_a = (uint8_t)(a & 0x07);
        *slp_b = (uint8_t)(b & 0x07);
        klog_puts("[acpi] \\_S5 read from the DSDT: SLP_TYPa=0x");
        klog_put_hex32(*slp_a);
        klog_puts(", SLP_TYPb=0x");
        klog_put_hex32(*slp_b);
        klog_puts(" - no longer a guess.\n");
        return 1;
    }
    klog_puts("[acpi] no readable \\_S5 in the DSDT - power off will try the well-known values.\n");
    return 0;
}

int acpi_find_madt(acpi_madt_info_t *out) {
    const acpi_sdt_header_t *madt = find_table("APIC");
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
