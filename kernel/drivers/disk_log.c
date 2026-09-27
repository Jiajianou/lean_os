#include "disk_log.h"

#include "boot/boot_options.h"
#include "drivers/block_device.h"
#include "drivers/disk_log_area.h"
#include "drivers/kernel_log.h"
#include "scheduler/scheduler.h"

#define DISK_LOG_INTERVAL_MS 1000u

static disk_log_area_t area;
static uint64_t cursor;
static char chunk[16384];

static int read_sectors(void *context, uint32_t lba, uint32_t count, void *buffer) {
    (void)context;
    return block_device_read(lba, count, buffer);
}

static int write_sectors(void *context, uint32_t lba, uint32_t count, void *buffer) {
    (void)context;
    return block_device_write(lba, count, buffer);
}

static int append_text(const char *text) {
    size_t length = 0;
    while (text[length]) {
        length++;
    }
    return disk_log_area_append(&area, text, length);
}

static int append_decimal(uint64_t value) {
    char digits[21];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    char text[21];
    for (int i = 0; i < count; i++) {
        text[i] = digits[count - 1 - i];
    }
    return disk_log_area_append(&area, text, (size_t)count);
}

static int flush_once(void) {
    uint64_t total = kernel_log_written_total();
    if (cursor >= total) {
        return 0;
    }
    while (cursor < total) {
        uint64_t next = cursor;
        size_t n = kernel_log_read(cursor, chunk, sizeof(chunk), &next);
        uint64_t from = next - n;
        if (from > cursor) {
            if (append_text("\n[disk-log] ") != 0 || append_decimal(from - cursor) != 0 ||
                append_text(" bytes of the kernel log were overwritten before they reached the disk\n") != 0) {
                return -1;
            }
        }
        if (n == 0) {
            break;
        }
        if (disk_log_area_append(&area, chunk, n) != 0) {
            return -1;
        }
        cursor = next;
    }
    if (disk_log_area_write_header(&area) != 0) {
        return -1;
    }
    return block_device_flush();
}

static void disk_log_task(void *argument) {
    (void)argument;
    int failures = 0;
    for (;;) {
        /* A second is short enough that a machine which freezes still leaves
           the last thing it said on the stick, and long enough that the
           writes are a trickle on a 5 MB/s USB stick. */
        if (flush_once() != 0) {
            failures++;
            if (failures == 3) {
                kernel_log_puts("[disk-log] three writes to the log area failed - stopping.\n");
                return;
            }
        } else {
            failures = 0;
        }
        scheduler_sleep_ms(DISK_LOG_INTERVAL_MS);
    }
}

void disk_log_start(void) {
    const boot_options_t *options = boot_options_active();
    if (options->log_sectors == 0) {
        return;
    }
    int opened = disk_log_area_open(&area, options->log_lba, options->log_sectors, read_sectors,
                                    write_sectors, 0);
    if (opened == DISK_LOG_AREA_NOT_AN_AREA) {
        /* The LBA came from a text file anyone can edit. Writing there without
           finding the header the image tool put there would be writing over
           whatever is there instead. */
        kernel_log_puts("[disk-log] LBA ");
        kernel_log_put_dec(options->log_lba);
        kernel_log_puts(" does not start with the log area's header - not writing there.\n");
        return;
    }
    if (opened != DISK_LOG_AREA_OPENED) {
        kernel_log_puts("[disk-log] could not read the log area - no log this boot.\n");
        return;
    }
    kernel_log_puts("[disk-log] mirroring the kernel log to LBA ");
    kernel_log_put_dec(options->log_lba);
    kernel_log_puts(" (");
    kernel_log_put_dec(area.data_sectors / 2u);
    kernel_log_puts(" KiB), boot ");
    kernel_log_put_dec(area.boots);
    kernel_log_puts(" on this disk - \\LOGS\\LEANOS.LOG on the EFI partition.\n");

    cursor = 0;
    if (append_text("\n===== lean_os boot ") != 0 || append_decimal(area.boots) != 0 ||
        append_text(" =====\n") != 0) {
        kernel_log_puts("[disk-log] the first write to the log area failed - no log this boot.\n");
        return;
    }
    task_t *writer = task_spawn("disk_log", disk_log_task, 0);
    if (writer) {
        writer->parent_id = -1;
    }
}
