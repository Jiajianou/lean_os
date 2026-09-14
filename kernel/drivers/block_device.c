#include "block_device.h"

#include "drivers/ahci.h"
#include "drivers/ata.h"
#include "drivers/kernel_log.h"
#include "drivers/nvme.h"
#include "drivers/virtio_block.h"
#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "drivers/pit.h"
#include "memory_management/physical_memory.h"

#define BLOCK_DEVICE_PER_LINE 8
#define LINE_BYTES   (BLOCK_DEVICE_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE)

#define CACHE_LINES 2048
#define SCRATCH_LINES 16

typedef struct {
    uint32_t line_no;
    uint8_t  valid;
    uint8_t  dirty;
    uint64_t dirty_tick;
} cache_tag_t;

#define BLOCK_DEVICE_FLUSH_DEADLINE_MS 5000

static cache_tag_t tags[CACHE_LINES];
static uint8_t *line_pointer[CACHE_LINES];
static uint32_t cache_lines;
static uint64_t oldest_dirty_ms;
static uint8_t *scratch;
static uint32_t scratch_lines;

typedef enum {
    BACKEND_NVME,
    BACKEND_AHCI,
    BACKEND_VIRTIO,
    BACKEND_ATA,
} block_device_backend_t;

static block_device_backend_t backend = BACKEND_ATA;
static block_device_statistics_t statistics;

static spinlock_t block_device_lock;

static int64_t fault_reads_after = -1;
static int64_t fault_writes_after = -1;
static uint64_t reads_issued;
static uint64_t writes_issued;
static uint64_t io_errors;

void block_device_fault_inject(int64_t fail_reads_after, int64_t fail_writes_after) {
    fault_reads_after = fail_reads_after;
    fault_writes_after = fail_writes_after;
    reads_issued = 0;
    writes_issued = 0;
}

uint64_t block_device_error_count(void) {
    return io_errors;
}

static int device_read(uint32_t lba, uint32_t count, void *buffer) {
    statistics.device_reads += count;
    if (fault_reads_after >= 0 && (int64_t)reads_issued++ >= fault_reads_after) {
        io_errors++;
        return -1;
    }
    if (backend == BACKEND_NVME) {
        if (nvme_read(lba, count, buffer) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    if (backend == BACKEND_AHCI) {
        if (ahci_read(lba, count, buffer) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    if (backend == BACKEND_VIRTIO) {
        if (virtio_block_device_read(lba, count, buffer) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    uint8_t *destination = (uint8_t *)buffer;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        if (ata_read_sectors(lba, (uint8_t)n, destination) != 0) {
            io_errors++;
            return -1;
        }
        destination += n * BLOCK_DEVICE_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static int device_write(uint32_t lba, uint32_t count, const void *buffer) {
    statistics.device_writes += count;
    if (fault_writes_after >= 0 && (int64_t)writes_issued++ >= fault_writes_after) {
        io_errors++;
        return -1;
    }
    if (backend == BACKEND_NVME) {
        if (nvme_write(lba, count, buffer) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    if (backend == BACKEND_AHCI) {
        if (ahci_write(lba, count, buffer) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    if (backend == BACKEND_VIRTIO) {
        if (virtio_block_device_write(lba, count, buffer) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    const uint8_t *source = (const uint8_t *)buffer;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        if (ata_write_sectors(lba, (uint8_t)n, source) != 0) {
            io_errors++;
            return -1;
        }
        source += n * BLOCK_DEVICE_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

void block_device_init(void) {
    if (nvme_init()) {
        backend = BACKEND_NVME;
    } else if (ahci_init()) {
        backend = BACKEND_AHCI;
    } else if (virtio_block_device_init()) {
        backend = BACKEND_VIRTIO;
    } else {
        backend = BACKEND_ATA;
    }

    cache_lines = CACHE_LINES;
    uint64_t affordable = physical_memory_free_frame_count() / 16;
    if (affordable < cache_lines) {
        cache_lines = (uint32_t)affordable;
    }
    if (cache_lines < 64) {
        cache_lines = 64;
    }

    uint32_t got = 0;
    for (uint32_t i = 0; i < cache_lines; i++) {
        uint64_t frame = physical_memory_try_alloc_frame();
        if (!frame) {
            break;
        }
        line_pointer[i] = (uint8_t *)(uintptr_t)frame;
        tags[i].valid = 0;
        got++;
    }
    cache_lines = got;
    statistics.capacity = cache_lines;

    scratch_lines = 0;
    uint64_t scratch_frame = physical_memory_try_alloc_contiguous(SCRATCH_LINES);
    if (scratch_frame) {
        scratch = (uint8_t *)(uintptr_t)scratch_frame;
        scratch_lines = SCRATCH_LINES;
    }

    kernel_log_puts("[blk] ");
    kernel_log_puts(block_device_backend_name());
    kernel_log_puts(", ");
    kernel_log_put_dec((cache_lines * LINE_BYTES) / 1024);
    kernel_log_puts(" KiB write-through cache in ");
    kernel_log_put_dec(cache_lines);
    kernel_log_puts(" lines of ");
    kernel_log_put_dec(LINE_BYTES);
    kernel_log_puts(" bytes\n");
}

const char *block_device_backend_name(void) {
    switch (backend) {
    case BACKEND_NVME:   return "nvme";
    case BACKEND_AHCI:   return "ahci";
    case BACKEND_VIRTIO: return "virtio-blk";
    default:             return "ata-pio";
    }
}

static int flush_line(uint32_t s);
static int flush_all_locked(void);
static void flush_if_overdue_locked(void);

static uint32_t slot_of(uint32_t line_no) {
    return line_no % cache_lines;
}

static int all_lines_resident(uint32_t first_line, uint32_t last_line) {
    for (uint32_t ln = first_line; ln <= last_line; ln++) {
        uint32_t s = slot_of(ln);
        if (!(tags[s].valid && tags[s].line_no == ln)) {
            return 0;
        }
    }
    return 1;
}

static uint32_t readahead_lines = 0;
static uint32_t last_read_end;

void block_device_set_readahead(uint32_t lines) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    readahead_lines = lines;
    spin_unlock_irqrestore(&block_device_lock, irq);
}

int block_device_read(uint32_t lba, uint32_t count, void *buffer) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    flush_if_overdue_locked();
    int sequential = (lba == last_read_end);
    last_read_end = lba + count;
    statistics.reads++;
    uint8_t *destination = (uint8_t *)buffer;
    uint32_t first_line = lba / BLOCK_DEVICE_PER_LINE;
    uint32_t last_line = (lba + count - 1) / BLOCK_DEVICE_PER_LINE;

    if (all_lines_resident(first_line, last_line)) {
        statistics.hits++;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = slot_of((lba + i) / BLOCK_DEVICE_PER_LINE);
            uint32_t within = (lba + i) % BLOCK_DEVICE_PER_LINE;
            k_memcpy(destination + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE,
                     line_pointer[s] + (uint64_t)within * BLOCK_DEVICE_SECTOR_SIZE,
                     BLOCK_DEVICE_SECTOR_SIZE);
        }
        spin_unlock_irqrestore(&block_device_lock, irq);
        return 0;
    }

    uint32_t span_lines = last_line - first_line + 1;
    uint32_t span_lba = first_line * BLOCK_DEVICE_PER_LINE;
    uint32_t span_count = span_lines * BLOCK_DEVICE_PER_LINE;
    int filled = 0;

    if (scratch && span_lines <= scratch_lines &&
        (span_lba != lba || span_count != count) &&
        device_read(span_lba, span_count, scratch) == 0) {
        for (uint32_t k = 0; k < span_lines; k++) {
            uint32_t ln = first_line + k;
            uint32_t s = slot_of(ln);
            uint8_t *in_scratch = scratch + (uint64_t)k * LINE_BYTES;
            if (tags[s].valid && tags[s].line_no == ln && tags[s].dirty) {
                k_memcpy(in_scratch, line_pointer[s], LINE_BYTES);
                continue;
            }
            if (tags[s].valid && tags[s].dirty && tags[s].line_no != ln) {
                flush_line(s);
            }
            if (!tags[s].valid) {
                statistics.resident++;
            }
            k_memcpy(line_pointer[s], in_scratch, LINE_BYTES);
            tags[s].line_no = ln;
            tags[s].valid = 1;
            tags[s].dirty = 0;
        }
        k_memcpy(destination, scratch + (uint64_t)(lba - span_lba) * BLOCK_DEVICE_SECTOR_SIZE,
                 (uint64_t)count * BLOCK_DEVICE_SECTOR_SIZE);
        filled = 1;
    }

    if (!filled) {
        if (device_read(lba, count, destination) != 0) {
            k_memset(destination, 0, (uint64_t)count * BLOCK_DEVICE_SECTOR_SIZE);
            spin_unlock_irqrestore(&block_device_lock, irq);
            return -1;
        }

        for (uint32_t ln = first_line; ln <= last_line; ln++) {
            uint32_t line_lba = ln * BLOCK_DEVICE_PER_LINE;
            if (line_lba < lba || line_lba + BLOCK_DEVICE_PER_LINE > lba + count) {
                continue;
            }
            uint32_t s = slot_of(ln);
            if (tags[s].valid && tags[s].line_no == ln && tags[s].dirty) {
                k_memcpy(destination + (uint64_t)(line_lba - lba) * BLOCK_DEVICE_SECTOR_SIZE,
                         line_pointer[s], LINE_BYTES);
                continue;
            }
            if (!tags[s].valid) {
                statistics.resident++;
            }
            if (tags[s].dirty && tags[s].line_no != ln) {
                flush_line(s);
            }
            k_memcpy(line_pointer[s], destination + (uint64_t)(line_lba - lba) * BLOCK_DEVICE_SECTOR_SIZE, LINE_BYTES);
            tags[s].line_no = ln;
            tags[s].valid = 1;
            tags[s].dirty = 0;
        }
    }

    if (sequential && readahead_lines > 0) {
        for (uint32_t k = 0; k < readahead_lines; k++) {
            uint32_t ln = last_line + 1 + k;
            uint32_t s = slot_of(ln);
            if (tags[s].valid && tags[s].line_no == ln) {
                continue;
            }
            if (tags[s].dirty) {
                break;
            }
            if (device_read(ln * BLOCK_DEVICE_PER_LINE, BLOCK_DEVICE_PER_LINE, line_pointer[s]) != 0) {
                break;
            }
            if (!tags[s].valid) {
                statistics.resident++;
            }
            tags[s].line_no = ln;
            tags[s].valid = 1;
            tags[s].dirty = 0;
            statistics.readaheads++;
        }
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
    return 0;
}

static int flush_line(uint32_t s) {
    if (!tags[s].valid || !tags[s].dirty) {
        return 0;
    }
    if (device_write(tags[s].line_no * BLOCK_DEVICE_PER_LINE, BLOCK_DEVICE_PER_LINE, line_pointer[s]) != 0) {
        return -1;
    }
    tags[s].dirty = 0;
    statistics.writebacks++;
    return 0;
}

static int line_is_dirty(uint32_t line_no) {
    uint32_t s = slot_of(line_no);
    return tags[s].valid && tags[s].dirty && tags[s].line_no == line_no;
}

static int flush_run(uint32_t first, uint32_t run) {
    for (uint32_t k = 0; k < run; k++) {
        k_memcpy(scratch + (uint64_t)k * LINE_BYTES,
                 line_pointer[slot_of(first + k)], LINE_BYTES);
    }
    if (device_write(first * BLOCK_DEVICE_PER_LINE, run * BLOCK_DEVICE_PER_LINE,
                     scratch) != 0) {
        return -1;
    }
    for (uint32_t k = 0; k < run; k++) {
        tags[slot_of(first + k)].dirty = 0;
    }
    statistics.writebacks += run;
    return 0;
}

static int flush_all_locked(void) {
    if (statistics.dirty == 0) {
        return 0;
    }
    uint32_t still_dirty = 0;
    for (uint32_t i = 0; i < cache_lines; i++) {
        if (!tags[i].valid || !tags[i].dirty) {
            continue;
        }
        if (!scratch || scratch_lines < 2) {
            if (flush_line(i) != 0) {
                still_dirty++;
            }
            continue;
        }
        uint32_t first = tags[i].line_no;
        while (first > 0 && line_is_dirty(first - 1) &&
               (tags[i].line_no - first) + 1 < scratch_lines) {
            first--;
        }
        uint32_t run = 1;
        while (run < scratch_lines && line_is_dirty(first + run)) {
            run++;
        }
        if (flush_run(first, run) != 0) {
            for (uint32_t k = 0; k < run; k++) {
                if (line_is_dirty(first + k)) {
                    still_dirty++;
                }
            }
        }
    }
    statistics.dirty = still_dirty;
    return still_dirty == 0 ? 0 : -1;
}

static void flush_if_overdue_locked(void) {
    if (statistics.dirty == 0) {
        return;
    }
    uint64_t now = pit_get_ticks() * (1000 / PIT_HZ);
    if (now >= oldest_dirty_ms + BLOCK_DEVICE_FLUSH_DEADLINE_MS) {
        flush_all_locked();
    }
}

int block_device_write(uint32_t lba, uint32_t count, const void *buffer) {
    int failed = 0;
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    flush_if_overdue_locked();
    const uint8_t *source = (const uint8_t *)buffer;

    uint32_t i = 0;
    while (i < count) {
        uint32_t line_no = (lba + i) / BLOCK_DEVICE_PER_LINE;
        uint32_t within = (lba + i) % BLOCK_DEVICE_PER_LINE;
        uint32_t remaining = count - i;
        int whole_line = (within == 0 && remaining >= BLOCK_DEVICE_PER_LINE);

        if (whole_line) {
            uint32_t s = slot_of(line_no);
            if (tags[s].valid && tags[s].dirty && tags[s].line_no != line_no) {
                if (flush_line(s) != 0) {
                    if (device_write(lba + i, BLOCK_DEVICE_PER_LINE,
                                     source + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE) != 0) {
                        failed = 1;
                    }
                    i += BLOCK_DEVICE_PER_LINE;
                    continue;
                }
            }
            if (!tags[s].valid) {
                statistics.resident++;
            }
            k_memcpy(line_pointer[s], source + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE, LINE_BYTES);
            if (!tags[s].dirty) {
                if (statistics.dirty == 0) {
                    oldest_dirty_ms = pit_get_ticks() * (1000 / PIT_HZ);
                }
                statistics.dirty++;
                tags[s].dirty_tick = oldest_dirty_ms;
            }
            tags[s].valid = 1;
            tags[s].line_no = line_no;
            tags[s].dirty = 1;
            i += BLOCK_DEVICE_PER_LINE;
            continue;
        }

        uint32_t run = BLOCK_DEVICE_PER_LINE - within;
        if (run > remaining) {
            run = remaining;
        }
        if (device_write(lba + i, run, source + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE) != 0) {
            failed = 1;
        }
        uint32_t s = slot_of(line_no);
        if (tags[s].valid && tags[s].line_no == line_no) {
            k_memcpy(line_pointer[s] + (uint64_t)within * BLOCK_DEVICE_SECTOR_SIZE,
                     source + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE,
                     (uint64_t)run * BLOCK_DEVICE_SECTOR_SIZE);
        }
        i += run;
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
    return failed ? -1 : 0;
}

int block_device_flush(void) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    int r = flush_all_locked();
    spin_unlock_irqrestore(&block_device_lock, irq);
    return r;
}

void block_device_statistics(block_device_statistics_t *out) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    *out = statistics;
    spin_unlock_irqrestore(&block_device_lock, irq);
}

void block_device_cache_drop(void) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    flush_all_locked();
    for (uint32_t i = 0; i < cache_lines; i++) {
        tags[i].valid = 0;
        tags[i].dirty = 0;
    }
    statistics.resident = 0;
    statistics.dirty = 0;
    spin_unlock_irqrestore(&block_device_lock, irq);
}
