#include "fwcfg.h"

#include "architecture/x86_64/io.h"
#include "drivers/kernel_log.h"
#include "library/kernel_library.h"

#define FWCFG_PORT_SEL  0x510
#define FWCFG_PORT_DATA 0x511

#define FWCFG_SIGNATURE 0x0000
#define FWCFG_FILE_DIRECTORY  0x0019

#define FWCFG_NAME_LENGTH 56

static int present;

static void fwcfg_select(uint16_t item) {
    outw(FWCFG_PORT_SEL, item);
}

static void fwcfg_read(void *destination, uint32_t length) {
    uint8_t *out = (uint8_t *)destination;
    for (uint32_t i = 0; i < length; i++) {
        out[i] = inb(FWCFG_PORT_DATA);
    }
}

static void fwcfg_skip(uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
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

static int selftest_cached = -1;
static int ioapic_cached = -1;
static int bootstrap_cached = -1;
static int pybuild_cached = -1;
static int pytest_cached = -1;

void fwcfg_init(void) {
    selftest_cached = -1;
    ioapic_cached = -1;
    bootstrap_cached = -1;
    pybuild_cached = -1;
    pytest_cached = -1;

    uint8_t sig[4];
    fwcfg_select(FWCFG_SIGNATURE);
    fwcfg_read(sig, sizeof(sig));
    present = (sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U');

    kernel_log_puts("[fwcfg] ");
    if (present) {
        kernel_log_puts("QEMU firmware config device present.\n");
    } else {
        kernel_log_puts("no firmware config device - self-tests default to off.\n");
    }
}

int fwcfg_present(void) {
    return present;
}

int fwcfg_read_file(const char *name, void *destination, uint32_t max) {
    if (!present || !name || !destination) {
        return -1;
    }

    uint8_t header[4];
    fwcfg_select(FWCFG_FILE_DIRECTORY);
    fwcfg_read(header, sizeof(header));
    uint32_t count = be32(header);

    if (count > 1024) {
        return -1;
    }

    uint32_t found_size = 0;
    uint16_t found_sel = 0;
    int found = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t entry[8];
        char entry_name[FWCFG_NAME_LENGTH];
        fwcfg_read(entry, sizeof(entry));
        fwcfg_read(entry_name, sizeof(entry_name));

        if (found) {
            continue;
        }

        uint32_t n = 0;
        while (n < FWCFG_NAME_LENGTH && name[n] != '\0' && entry_name[n] == name[n]) {
            n++;
        }
        if (name[n] == '\0' && (n == FWCFG_NAME_LENGTH || entry_name[n] == '\0')) {
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
    fwcfg_read(destination, want);
    if (found_size > want) {
        fwcfg_skip(found_size - want);
    }
    return (int)want;
}

int boot_ioapic_enabled(void) {
    if (ioapic_cached >= 0) {
        return ioapic_cached;
    }
    char buffer[8];
    k_memset(buffer, 0, sizeof(buffer));
    int n = fwcfg_read_file("opt/leanos/ioapic", buffer, sizeof(buffer) - 1);
    ioapic_cached = (n == 1 && buffer[0] == '1') ? 1 : 0;
    return ioapic_cached;
}

int boot_bootstrap_enabled(void) {
    if (bootstrap_cached >= 0) {
        return bootstrap_cached;
    }
    char buffer[8];
    k_memset(buffer, 0, sizeof(buffer));
    int n = fwcfg_read_file("opt/leanos/bootstrap", buffer, sizeof(buffer) - 1);
    bootstrap_cached = (n == 1 && buffer[0] == '1') ? 1 : 0;
    return bootstrap_cached;
}

int boot_pytest_enabled(void) {
    if (pytest_cached >= 0) {
        return pytest_cached;
    }
    char buffer[8];
    k_memset(buffer, 0, sizeof(buffer));
    int n = fwcfg_read_file("opt/leanos/pytest", buffer, sizeof(buffer) - 1);
    pytest_cached = (n == 1 && buffer[0] == '1') ? 1 : 0;
    return pytest_cached;
}

int boot_pybuild_enabled(void) {
    if (pybuild_cached >= 0) {
        return pybuild_cached;
    }
    char buffer[8];
    k_memset(buffer, 0, sizeof(buffer));
    int n = fwcfg_read_file("opt/leanos/pybuild", buffer, sizeof(buffer) - 1);
    pybuild_cached = (n == 1 && buffer[0] == '1') ? 1 : 0;
    return pybuild_cached;
}

int boot_selftests_enabled(void) {
    if (selftest_cached >= 0) {
        return selftest_cached;
    }

    char buffer[8];
    k_memset(buffer, 0, sizeof(buffer));
    int n = fwcfg_read_file("opt/leanos/selftest", buffer, sizeof(buffer) - 1);

    selftest_cached = (n == 1 && buffer[0] == '1') ? 1 : 0;
    return selftest_cached;
}
