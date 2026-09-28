#include "disk_log.h"

#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "architecture/x86_64/timestamp_counter.h"
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

/* M193. The first log home from the laptop said the browser was slow and
   could not say where the time went: the kernel log has no clock in it. The
   file gets one - a line a second while anything is happening, with the time
   since boot, what the disk did in that second and how busy each processor
   was - and the serial port does not, because harnesses grep its lines. A
   stamp only goes in at a line boundary so it never splits somebody's line. */
static uint64_t stamp_last_us;
static uint64_t stamp_last_read_sectors;
static uint64_t stamp_last_written_sectors;
static uint64_t stamp_last_total[MAX_CPUS];
static uint64_t stamp_last_idle[MAX_CPUS];
static int at_line_start = 1;

static int append_stamp(int log_grew) {
    block_device_statistics_t disk;
    block_device_statistics(&disk);
    uint64_t now_us = tsc_to_us(tsc_read());
    uint64_t read_sectors = disk.device_reads - stamp_last_read_sectors;
    uint64_t written_sectors = disk.device_writes - stamp_last_written_sectors;
    uint32_t busy_percent[MAX_CPUS];
    int any_busy = 0;
    int cpus = smp_cpu_count > 0 && smp_cpu_count <= MAX_CPUS ? smp_cpu_count : 1;
    for (int c = 0; c < cpus; c++) {
        uint64_t total = scheduler_total_ticks(c) - stamp_last_total[c];
        uint64_t idle = scheduler_idle_ticks(c) - stamp_last_idle[c];
        busy_percent[c] = total ? (uint32_t)(((total - (idle < total ? idle : total)) * 100u) / total) : 0;
        if (busy_percent[c] >= 10) {
            any_busy = 1;
        }
    }
    if (!log_grew && !read_sectors && !written_sectors && !any_busy) {
        return 0;
    }
    if (append_text("[t+") != 0 || append_decimal(now_us / 1000000u) != 0 ||
        append_text(".") != 0 || append_decimal((now_us / 100000u) % 10u) != 0 ||
        append_text("s] disk read ") != 0 || append_decimal(read_sectors / 2u) != 0 ||
        append_text(" KiB, written ") != 0 || append_decimal(written_sectors / 2u) != 0 ||
        append_text(" KiB in ") != 0 ||
        append_decimal((now_us - stamp_last_us) / 1000u) != 0 || append_text(" ms; cpu busy %") != 0) {
        return -1;
    }
    for (int c = 0; c < cpus; c++) {
        if (append_text(" ") != 0 || append_decimal(busy_percent[c]) != 0) {
            return -1;
        }
    }
    if (append_text("\n") != 0) {
        return -1;
    }
    stamp_last_us = now_us;
    stamp_last_read_sectors = disk.device_reads;
    stamp_last_written_sectors = disk.device_writes;
    for (int c = 0; c < cpus; c++) {
        stamp_last_total[c] = scheduler_total_ticks(c);
        stamp_last_idle[c] = scheduler_idle_ticks(c);
    }
    return 0;
}

static int flush_once(void) {
    uint64_t total = kernel_log_written_total();
    if (at_line_start && append_stamp(cursor < total) != 0) {
        return -1;
    }
    if (cursor >= total) {
        return disk_log_area_write_header(&area) != 0 ? -1 : block_device_flush();
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
        at_line_start = chunk[n - 1] == '\n';
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
