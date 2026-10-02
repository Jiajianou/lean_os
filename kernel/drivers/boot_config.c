#include "boot_config.h"

#include "boot/boot_config_edit.h"
#include "drivers/block_device.h"
#include "drivers/kernel_log.h"
#include "memory_management/heap.h"

#define SECTOR_BYTES 512u

/* M213. The sectors config= names came out of a text file anybody can
   edit, so nothing is written there unless what is there already starts with
   the header the image tool wrote - the same rule the disk log keeps. Only
   one rewrite runs at a time; a second caller is told to come back rather
   than made to wait behind a USB stick. */
static int busy;

static int claim(void) {
    int expected = 0;
    return __atomic_compare_exchange_n(&busy, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void release(void) {
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

static char *read_area(uint32_t *bytes) {
    const boot_options_t *options = boot_options_active();
    if (options->config_lba == 0 || options->config_sectors == 0) {
        return 0;
    }
    *bytes = options->config_sectors * SECTOR_BYTES;
    char *area = (char *)kmalloc(*bytes);
    if (!area) {
        return 0;
    }
    if (block_device_read(options->config_lba, options->config_sectors, area) != 0 ||
        !boot_config_is_ours(area, *bytes)) {
        kfree(area);
        return 0;
    }
    return area;
}

int boot_config_available(void) {
    if (!claim()) {
        return 1;
    }
    uint32_t bytes = 0;
    char *area = read_area(&bytes);
    release();
    if (!area) {
        return 0;
    }
    kfree(area);
    return 1;
}

int boot_config_startup(boot_options_t *out) {
    boot_options_defaults(out);
    if (!claim()) {
        return -1;
    }
    uint32_t bytes = 0;
    char *area = read_area(&bytes);
    release();
    if (!area) {
        return -1;
    }
    boot_options_parse(area, bytes, out);
    kfree(area);
    return 0;
}

static void decimal(uint32_t value, char *out) {
    char digits[12];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < 11);
    for (int i = 0; i < n; i++) {
        out[i] = digits[n - 1 - i];
    }
    out[n] = '\0';
}

int boot_config_set_startup_mode(uint32_t width, uint32_t height) {
    char value[32];
    if (width == 0 && height == 0) {
        const char native[] = "native";
        for (uint32_t i = 0; i < sizeof(native); i++) {
            value[i] = native[i];
        }
    } else {
        if (width == 0 || height == 0 || width > 16384 || height > 16384) {
            return -1;
        }
        decimal(width, value);
        uint32_t at = 0;
        while (value[at]) {
            at++;
        }
        value[at++] = 'x';
        decimal(height, value + at);
    }

    if (!claim()) {
        return -1;
    }
    uint32_t bytes = 0;
    char *area = read_area(&bytes);
    int result = -1;
    if (area && boot_config_set(area, bytes, "video", value) > 0) {
        const boot_options_t *options = boot_options_active();
        if (block_device_write(options->config_lba, options->config_sectors, area) == 0 &&
            block_device_flush() == 0) {
            result = 0;
            kernel_log_puts("[display] the next boot starts in video=");
            kernel_log_puts(value);
            kernel_log_puts(", written to \\EFI\\BOOT\\lean_os.cfg.\n");
        }
    }
    if (area) {
        kfree(area);
    }
    release();
    return result;
}
