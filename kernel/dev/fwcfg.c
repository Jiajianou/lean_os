#include "fwcfg.h"

#include "arch/x86_64/io.h"
#include "drivers/klog.h"
#include "lib/libk.h"

#define FWCFG_PORT_SEL  0x510
#define FWCFG_PORT_DATA 0x511

#define FWCFG_SIGNATURE 0x0000
#define FWCFG_FILE_DIR  0x0019

/* 56 bytes of name plus the three fixed fields - the on-wire entry is
 * exactly 64 bytes and this kernel reads it a byte at a time rather than
 * casting a struct over it, because the fields are big-endian and a
 * packed struct would only hide that. */
#define FWCFG_NAME_LEN 56

static int present;

static void fwcfg_select(uint16_t item) {
    outw(FWCFG_PORT_SEL, item);
}

static void fwcfg_read(void *dst, uint32_t len) {
    uint8_t *out = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++) {
        out[i] = inb(FWCFG_PORT_DATA);
    }
}

/* Skips forward within the currently selected item. The device has no
 * seek, so the only way past a field is to read it. */
static void fwcfg_skip(uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        (void)inb(FWCFG_PORT_DATA);
    }
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Reset by fwcfg_init - see boot_selftests_enabled and
 * boot_ioapic_enabled. */
static int selftest_cached = -1;
/* M103 - see boot_ioapic_enabled. */
static int ioapic_cached = -1;
/* M98 - see boot_bootstrap_enabled. */
static int bootstrap_cached = -1;

void fwcfg_init(void) {
    /* Q11: a re-probe invalidates the cached answer. On the machine
     * fwcfg_init runs once and this is invisible; it is correct anyway,
     * because a cache that survives a re-probe is a cache that reports
     * the previous device's answer about this one. */
    selftest_cached = -1;
    ioapic_cached = -1; /* M103 */
    bootstrap_cached = -1; /* M98 */

    uint8_t sig[4];
    fwcfg_select(FWCFG_SIGNATURE);
    fwcfg_read(sig, sizeof(sig));
    present = (sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U');

    klog_puts("[fwcfg] ");
    if (present) {
        klog_puts("QEMU firmware config device present.\n");
    } else {
        /* Not a warning. This is what real hardware looks like. */
        klog_puts("no firmware config device - self-tests default to off.\n");
    }
}

int fwcfg_present(void) {
    return present;
}

int fwcfg_read_file(const char *name, void *dst, uint32_t max) {
    if (!present || !name || !dst) {
        return -1;
    }

    uint8_t hdr[4];
    fwcfg_select(FWCFG_FILE_DIR);
    fwcfg_read(hdr, sizeof(hdr));
    uint32_t count = be32(hdr);

    /* A sanity ceiling rather than a trusted count: this number comes
     * from outside the machine, and a corrupt or hostile one would
     * otherwise turn into an unbounded loop of port reads. QEMU's own
     * directory is a few dozen entries. */
    if (count > 1024) {
        return -1;
    }

    uint32_t found_size = 0;
    uint16_t found_sel = 0;
    int found = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t entry[8];
        char entry_name[FWCFG_NAME_LEN];
        fwcfg_read(entry, sizeof(entry));
        fwcfg_read(entry_name, sizeof(entry_name));

        if (found) {
            /* The whole directory has to be read even after a match -
             * there is no seek, and leaving the stream mid-item would
             * corrupt the next selection. Keep going, ignore the rest. */
            continue;
        }

        /* The name field is NUL-padded, not NUL-terminated, when it uses
         * all 56 bytes. Compare within the field rather than trusting a
         * terminator to be there. */
        uint32_t n = 0;
        while (n < FWCFG_NAME_LEN && name[n] != '\0' && entry_name[n] == name[n]) {
            n++;
        }
        if (name[n] == '\0' && (n == FWCFG_NAME_LEN || entry_name[n] == '\0')) {
            found_size = be32(entry);
            found_sel = be16(entry + 4);
            found = 1;
        }
    }

    if (!found) {
        return -1;
    }

    uint32_t want = found_size < max ? found_size : max;
    fwcfg_select(found_sel);
    fwcfg_read(dst, want);
    if (found_size > want) {
        fwcfg_skip(found_size - want);
    }
    return (int)want;
}

/* ---- M103: which interrupt controller this boot uses ------------------
 *
 * Same mechanism, same reasons, and the same cached shape as the
 * self-test switch below. What it selects and WHY the default is what it
 * is are in fwcfg.h, next to the measurement.
 */
int boot_ioapic_enabled(void) {
    if (ioapic_cached >= 0) {
        return ioapic_cached;
    }
    char buf[8];
    k_memset(buf, 0, sizeof(buf));
    int n = fwcfg_read_file("opt/leanos/ioapic", buf, sizeof(buf) - 1);
    ioapic_cached = (n == 1 && buf[0] == '1') ? 1 : 0;
    return ioapic_cached;
}

int boot_bootstrap_enabled(void) {
    if (bootstrap_cached >= 0) {
        return bootstrap_cached;
    }
    char buf[8];
    k_memset(buf, 0, sizeof(buf));
    int n = fwcfg_read_file("opt/leanos/bootstrap", buf, sizeof(buf) - 1);
    bootstrap_cached = (n == 1 && buf[0] == '1') ? 1 : 0;
    return bootstrap_cached;
}

int boot_selftests_enabled(void) {
    /* Cached, because this is asked once per self-test region and the
     * answer cannot change during a boot. -1 is "not yet asked", and
     * fwcfg_init puts it back there. */
    if (selftest_cached >= 0) {
        return selftest_cached;
    }

    char buf[8];
    k_memset(buf, 0, sizeof(buf));
    int n = fwcfg_read_file("opt/leanos/selftest", buf, sizeof(buf) - 1);

    /* Exactly "1" turns them on. Not "any non-empty value": a typo in a
     * harness invocation should fail closed and be noticed, rather than
     * silently enabling a 140-second boot. QEMU's `string=` blobs are
     * not NUL-terminated, hence the length check rather than a strcmp. */
    selftest_cached = (n == 1 && buf[0] == '1') ? 1 : 0;
    return selftest_cached;
}
