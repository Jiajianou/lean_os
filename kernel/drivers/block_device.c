#include "block_device.h"

#include "drivers/ahci.h"
#include "drivers/ata.h"
#include "drivers/kernel_log.h"
#include "drivers/nvme.h"
#include "drivers/usb_storage.h"
#include "file_system/leanfs_format.h"
#include "drivers/virtio_block.h"
#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "drivers/pit.h"
#include "memory_management/physical_memory.h"
#include "panic.h"

#define BLOCK_DEVICE_PER_LINE 8
#define LINE_BYTES   (BLOCK_DEVICE_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE)

#define SCRATCH_LINES 256

typedef struct {
    uint32_t line_no;
    uint8_t  valid;
    uint8_t  dirty;
    uint64_t dirty_tick;
} cache_tag_t;

#define BLOCK_DEVICE_FLUSH_DEADLINE_MS 5000

static cache_tag_t *tags;
static uint8_t **line_pointer;
static uint32_t cache_lines;
static uint32_t allocated_lines;
static uint64_t oldest_dirty_ms;
static uint8_t *scratch;
static uint32_t scratch_lines;

typedef enum {
    BACKEND_NVME,
    BACKEND_AHCI,
    BACKEND_VIRTIO,
    BACKEND_ATA,
    BACKEND_USB,
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
    if (backend == BACKEND_USB) {
        if (!usb_storage_read(lba, count, buffer)) {
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
    if (backend == BACKEND_USB) {
        if (!usb_storage_write(lba, count, buffer)) {
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

/* Until M191 this was 2048 lines - 8 MiB - whatever the machine. Chromium's
   executable is 306 MB and every process it starts execs it again, so on a
   laptop with 18 GB and a USB stick each renderer read the whole thing off the
   stick: twenty seconds apiece. A machine with memory to spare spends a
   sixteenth of it here, which holds that binary; a smaller one spends a
   sixty-fourth, because a browser under QEMU's 4 GiB has none to spare. The
   old size is the floor either way. */
uint32_t block_device_cache_lines_for(uint64_t free_frames) {
    uint64_t lines = free_frames >= BLOCK_DEVICE_LARGE_MACHINE_FRAMES ? free_frames / 16
                                                                     : free_frames / 64;
    uint64_t floor = free_frames / 16 < 2048 ? free_frames / 16 : 2048;
    if (lines < floor) {
        lines = floor;
    }
    if (lines > BLOCK_DEVICE_MAX_CACHE_LINES) {
        lines = BLOCK_DEVICE_MAX_CACHE_LINES;
    }
    if (lines < 64) {
        lines = 64;
    }
    return (uint32_t)lines;
}

void block_device_init(void) {
    /* Which disks are here, in the order they were probed before M186. The
       order still decides ties; what it no longer decides on its own is which
       disk this OS runs from. */
    block_device_backend_t candidates[5];
    uint32_t candidate_count = 0;
    if (nvme_init()) {
        candidates[candidate_count++] = BACKEND_NVME;
    }
    if (ahci_init()) {
        candidates[candidate_count++] = BACKEND_AHCI;
    }
    if (virtio_block_device_init()) {
        candidates[candidate_count++] = BACKEND_VIRTIO;
    }
    if (usb_storage_init()) {
        candidates[candidate_count++] = BACKEND_USB;
    }
    candidates[candidate_count++] = BACKEND_ATA;

    /* A machine booted from a stick has two disks on it and only one of them
       is this OS's. Before M186 the answer was "whichever was probed first",
       which on a laptop is the internal drive - somebody else's. The boot
       sector this project writes carries a label, so ask each disk whose it
       is rather than guessing. */
    backend = candidates[0];
    int found_labelled = 0;
    for (uint32_t i = 0; i < candidate_count && !found_labelled; i++) {
        backend = candidates[i];
        uint8_t first[BLOCK_DEVICE_SECTOR_SIZE];
        if (device_read(0, 1, first) != 0) {
            continue;
        }
        if (leanfs_disk_carries_this_os(first)) {
            found_labelled = 1;
        }
    }
    if (!found_labelled) {
        backend = candidates[0];
        kernel_log_puts("[blk] no disk here carries this OS's boot sector - taking the first one "
                   "found, which leanfs will refuse to format.\n");
    } else if (candidate_count > 1) {
        kernel_log_puts("[blk] chose the disk carrying this OS's boot sector out of ");
        kernel_log_put_dec(candidate_count);
        kernel_log_puts(" the probe found.\n");
    }

    io_errors = 0;
    reads_issued = 0;
    writes_issued = 0;
    statistics.device_reads = 0;
    statistics.device_writes = 0;

    uint32_t wanted = block_device_cache_lines_for(physical_memory_free_frame_count());
    if (!tags) {
        uint64_t tag_pages = ((uint64_t)wanted * sizeof(cache_tag_t) + 4095) / 4096;
        uint64_t pointer_pages = ((uint64_t)wanted * sizeof(uint8_t *) + 4095) / 4096;
        uint64_t tag_frames = physical_memory_try_alloc_contiguous(tag_pages);
        uint64_t pointer_frames = physical_memory_try_alloc_contiguous(pointer_pages);
        if (!tag_frames || !pointer_frames) {
            panic("block_device_init: no memory for the cache's own tables");
        }
        tags = (cache_tag_t *)(uintptr_t)tag_frames;
        line_pointer = (uint8_t **)(uintptr_t)pointer_frames;
        for (uint32_t i = 0; i < wanted; i++) {
            uint64_t frame = physical_memory_try_alloc_frame();
            if (!frame) {
                break;
            }
            line_pointer[i] = (uint8_t *)(uintptr_t)frame;
            allocated_lines++;
        }
    }
    cache_lines = allocated_lines;
    for (uint32_t i = 0; i < cache_lines; i++) {
        tags[i].valid = 0;
        tags[i].dirty = 0;
    }
    statistics.capacity = cache_lines;

    if (!scratch) {
        uint64_t scratch_frame = physical_memory_try_alloc_contiguous(SCRATCH_LINES);
        if (scratch_frame) {
            scratch = (uint8_t *)(uintptr_t)scratch_frame;
            scratch_lines = SCRATCH_LINES;
        }
    }
    statistics.resident = 0;
    statistics.dirty = 0;

    kernel_log_puts("[blk] ");
    kernel_log_puts(block_device_backend_name());
    kernel_log_puts(", ");
    kernel_log_put_dec((uint32_t)(((uint64_t)cache_lines * LINE_BYTES) / 1024));
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
    case BACKEND_USB:    return "usb-storage";
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

static int read_locked(uint32_t lba, uint32_t count, void *buffer) {
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

    /* One request for the whole window. Asking for it a line at a time made
       readahead slower than none over USB, where every request is three
       round trips before a byte moves. */
    if (sequential && readahead_lines > 0 && scratch) {
        uint32_t window = readahead_lines < scratch_lines ? readahead_lines : scratch_lines;
        uint32_t run = 0;
        while (run < window) {
            uint32_t ln = last_line + 1 + run;
            uint32_t s = slot_of(ln);
            if ((tags[s].valid && tags[s].line_no == ln) || tags[s].dirty) {
                break;
            }
            run++;
        }
        if (run > 0 &&
            device_read((last_line + 1) * BLOCK_DEVICE_PER_LINE, run * BLOCK_DEVICE_PER_LINE,
                        scratch) == 0) {
            for (uint32_t k = 0; k < run; k++) {
                uint32_t ln = last_line + 1 + k;
                uint32_t s = slot_of(ln);
                k_memcpy(line_pointer[s], scratch + (uint64_t)k * LINE_BYTES, LINE_BYTES);
                if (!tags[s].valid) {
                    statistics.resident++;
                }
                tags[s].line_no = ln;
                tags[s].valid = 1;
                tags[s].dirty = 0;
                statistics.readaheads++;
            }
        }
    }
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

static int write_locked(uint32_t lba, uint32_t count, const void *buffer) {
    int failed = 0;
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
    return failed ? -1 : 0;
}

/* M194: the journal.

   leanfs used to order its writes with a barrier after every metadata block
   (M71, M104): each one flushed the cache and waited for the device. On a USB
   stick, where every write command costs about 17 ms whatever its size, that
   was 84 of the 113 seconds a browser took to show its first page under QEMU
   at the stick's rates - eighteen megabytes written as five thousand commands.

   Now a write inside the filesystem goes into a transaction held here, and
   nothing reaches the device until a commit. A commit writes the transaction
   into the journal area as one sequential run, then a commit block whose
   checksum covers it. A checkpoint - when the journal fills, when the machine
   has been idle, at shutdown - writes the latest version of every committed
   block home, in LBA order, coalesced. A mount replays whatever was committed
   and not yet checkpointed. So every state the disk can be found in is one
   that existed between two operations.

   Blocks are held in a map rather than as dirty lines of the cache, because
   the cache is direct-mapped and evicts: a line that had to be written home to
   make room would be a piece of an uncommitted transaction on the disk. */

#define JOURNAL_EMPTY_KEY 0xFFFFFFFFu
#define JOURNAL_MAP_SLOTS 32768u
#define JOURNAL_MAP_MAX_ENTRIES 4096u
#define JOURNAL_COMMIT_DELAY_MS 1000u
#define JOURNAL_IDLE_CHECKPOINT_MS 5000u

typedef struct {
    uint32_t line_no;
    uint8_t uncommitted;
    uint8_t *data;
} journal_entry_t;

static int journaled;
static uint32_t journal_fs_first_line;
static uint32_t journal_fs_end_line;
static uint32_t journal_first_line;
static uint32_t journal_blocks;
static uint32_t journal_head;
static uint32_t journal_start;
static uint64_t journal_sequence;
static uint64_t journal_start_sequence;
static int journal_header_on_disk;
static journal_entry_t *journal_map;
static uint32_t journal_map_used;
static uint32_t journal_map_limit;
static uint32_t journal_uncommitted;
static uint64_t journal_oldest_uncommitted_ms;
static uint64_t journal_last_commit_ms;
static uint32_t *journal_order;

/* The transaction's blocks come from a pool reserved once, when the journal
   attaches, rather than from the frame allocator as they are written. Fifty
   of the boot battery's audits count free frames either side of something
   and require the two numbers to match; a journal borrowing frames between
   them is not a leak, and a pool is how it stays out of their arithmetic. */
static uint8_t **journal_pool;
static uint32_t journal_pool_size;
static uint32_t journal_pool_free;
static uint8_t journal_block[LINE_BYTES];
static uint8_t journal_partial[LINE_BYTES];

static uint64_t journal_now_ms(void) {
    return pit_get_ticks() * (1000 / PIT_HZ);
}

static uint32_t journal_hash(uint32_t line_no) {
    return (line_no * 2654435761u) & (JOURNAL_MAP_SLOTS - 1);
}

static int journal_find(uint32_t line_no) {
    if (!journal_map || journal_map_used == 0) {
        return -1;
    }
    for (uint32_t probe = 0, i = journal_hash(line_no); probe < JOURNAL_MAP_SLOTS;
         probe++, i = (i + 1) & (JOURNAL_MAP_SLOTS - 1)) {
        if (journal_map[i].line_no == line_no) {
            return (int)i;
        }
        if (journal_map[i].line_no == JOURNAL_EMPTY_KEY) {
            return -1;
        }
    }
    return -1;
}

static int journal_insert(uint32_t line_no) {
    if (journal_map_used >= journal_map_limit) {
        return -1;
    }
    uint32_t i = journal_hash(line_no);
    while (journal_map[i].line_no != JOURNAL_EMPTY_KEY) {
        i = (i + 1) & (JOURNAL_MAP_SLOTS - 1);
    }
    if (journal_pool_free == 0) {
        return -1;
    }
    journal_map[i].line_no = line_no;
    journal_map[i].uncommitted = 0;
    journal_map[i].data = journal_pool[--journal_pool_free];
    journal_map_used++;
    return (int)i;
}

static uint32_t journal_lba(uint32_t block) {
    return (journal_first_line + block) * BLOCK_DEVICE_PER_LINE;
}

static int journal_write_header(uint32_t start, uint64_t start_sequence) {
    k_memset(journal_block, 0, BLOCK_DEVICE_SECTOR_SIZE);
    leanfs_journal_header_t *h = (leanfs_journal_header_t *)journal_block;
    h->magic = LEANFS_JOURNAL_HEADER_MAGIC;
    h->version = LEANFS_JOURNAL_VERSION;
    h->start_block = start;
    h->journal_blocks = journal_blocks;
    h->start_sequence = start_sequence;
    h->checksum = leanfs_journal_header_checksum(h);
    if (device_write(journal_lba(0), 1, journal_block) != 0) {
        return -1;
    }
    journal_start = start;
    journal_start_sequence = start_sequence;
    journal_header_on_disk = 1;
    return 0;
}

static void journal_sort_by_line(uint32_t *order, uint32_t n) {
    for (uint32_t gap = n / 2; gap > 0; gap /= 2) {
        for (uint32_t i = gap; i < n; i++) {
            uint32_t moving = order[i];
            uint32_t key = journal_map[moving].line_no;
            uint32_t j = i;
            while (j >= gap && journal_map[order[j - gap]].line_no > key) {
                order[j] = order[j - gap];
                j -= gap;
            }
            order[j] = moving;
        }
    }
}

static void journal_install_in_cache(uint32_t line_no, const uint8_t *data) {
    uint32_t s = slot_of(line_no);
    if (tags[s].valid && tags[s].dirty && tags[s].line_no != line_no) {
        return;
    }
    if (!tags[s].valid) {
        statistics.resident++;
    }
    k_memcpy(line_pointer[s], data, LINE_BYTES);
    tags[s].line_no = line_no;
    tags[s].valid = 1;
    tags[s].dirty = 0;
}

/* Every committed block home, lowest LBA first and in runs as long as the
   staging buffer, and then the header moved so a mount replays nothing. With
   include_uncommitted it writes those too, which is only for a transaction too
   large for the journal - home, unprotected, as everything was before M194. */
static int journal_write_home(int include_uncommitted) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < JOURNAL_MAP_SLOTS; i++) {
        if (journal_map[i].line_no != JOURNAL_EMPTY_KEY &&
            (include_uncommitted || !journal_map[i].uncommitted)) {
            journal_order[n++] = i;
        }
    }
    journal_sort_by_line(journal_order, n);
    uint32_t k = 0;
    while (k < n) {
        uint32_t run = 1;
        while (k + run < n && run < scratch_lines &&
               journal_map[journal_order[k + run]].line_no ==
                   journal_map[journal_order[k]].line_no + run) {
            run++;
        }
        for (uint32_t r = 0; r < run; r++) {
            k_memcpy(scratch + (uint64_t)r * LINE_BYTES, journal_map[journal_order[k + r]].data,
                     LINE_BYTES);
        }
        if (device_write(journal_map[journal_order[k]].line_no * BLOCK_DEVICE_PER_LINE,
                         run * BLOCK_DEVICE_PER_LINE, scratch) != 0) {
            return -1;
        }
        statistics.journal_home_writes += run;
        k += run;
    }

    uint32_t kept = 0;
    for (uint32_t k2 = 0; k2 < n; k2++) {
        journal_entry_t *e = &journal_map[journal_order[k2]];
        journal_install_in_cache(e->line_no, e->data);
        journal_pool[journal_pool_free++] = e->data;
        e->line_no = JOURNAL_EMPTY_KEY;
        e->data = 0;
        journal_map_used--;
    }
    if (include_uncommitted) {
        journal_uncommitted = 0;
    }
    /* Removing entries from an open-addressed table breaks the probe chains
       of whatever is left, so the uncommitted ones are put back afresh. */
    if (journal_map_used > 0) {
        for (uint32_t i = 0; i < JOURNAL_MAP_SLOTS; i++) {
            if (journal_map[i].line_no != JOURNAL_EMPTY_KEY) {
                journal_order[kept++] = i;
            }
        }
        journal_entry_t moved[64];
        uint32_t done = 0;
        while (done < kept) {
            uint32_t batch = kept - done > 64 ? 64 : kept - done;
            for (uint32_t b = 0; b < batch; b++) {
                moved[b] = journal_map[journal_order[done + b]];
                journal_map[journal_order[done + b]].line_no = JOURNAL_EMPTY_KEY;
            }
            for (uint32_t b = 0; b < batch; b++) {
                uint32_t i = journal_hash(moved[b].line_no);
                while (journal_map[i].line_no != JOURNAL_EMPTY_KEY) {
                    i = (i + 1) & (JOURNAL_MAP_SLOTS - 1);
                }
                journal_map[i] = moved[b];
            }
            done += batch;
        }
    }
    statistics.journal_checkpoints++;
    journal_head = 1;
    return journal_write_header(1, journal_sequence);
}

static int journal_commit_locked(void) {
    if (!journaled || journal_uncommitted == 0) {
        return 0;
    }
    /* Room first: a checkpoint rebuilds the table, which moves entries, so
       the order below is only collected once nothing else will move them. */
    uint32_t need = leanfs_journal_descriptor_blocks(journal_uncommitted) + journal_uncommitted + 1;
    if (need > journal_blocks - 1) {
        kernel_log_puts("[blk] a transaction larger than the journal - written home unprotected.\n");
        return journal_write_home(1);
    }
    if (journal_head + need > journal_blocks) {
        if (journal_write_home(0) != 0) {
            return -1;
        }
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < JOURNAL_MAP_SLOTS; i++) {
        if (journal_map[i].line_no != JOURNAL_EMPTY_KEY && journal_map[i].uncommitted) {
            journal_order[n++] = i;
        }
    }
    uint32_t descriptors = leanfs_journal_descriptor_blocks(n);
    if (!journal_header_on_disk && journal_write_header(journal_head, journal_sequence) != 0) {
        return -1;
    }

    /* Descriptors and blocks go out as one sequential stream through the
       staging buffer; the checksum is over exactly the bytes written. */
    uint32_t checksum = LEANFS_FNV1A_INIT;
    uint32_t block = journal_head;
    uint32_t staged = 0;
    uint32_t stream_first = block;
    for (uint32_t item = 0; item < descriptors + n; item++) {
        uint8_t *slot = scratch + (uint64_t)staged * LINE_BYTES;
        if (item < descriptors) {
            leanfs_journal_descriptor_t *d = (leanfs_journal_descriptor_t *)slot;
            k_memset(d, 0, LINE_BYTES);
            d->magic = LEANFS_JOURNAL_DESCRIPTOR_MAGIC;
            d->index = item;
            d->sequence = journal_sequence;
            d->count = n;
            for (uint32_t t = 0; t < LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR; t++) {
                uint32_t which = item * LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR + t;
                if (which >= n) {
                    break;
                }
                d->targets[t] = journal_map[journal_order[which]].line_no;
            }
        } else {
            k_memcpy(slot, journal_map[journal_order[item - descriptors]].data, LINE_BYTES);
        }
        checksum = leanfs_fnv1a(checksum, slot, LINE_BYTES);
        staged++;
        block++;
        if (staged == scratch_lines || item + 1 == descriptors + n) {
            if (device_write(journal_lba(stream_first), staged * BLOCK_DEVICE_PER_LINE, scratch) != 0) {
                return -1;
            }
            statistics.journal_blocks_written += staged;
            staged = 0;
            stream_first = block;
        }
    }

    k_memset(journal_block, 0, LINE_BYTES);
    leanfs_journal_commit_t *c = (leanfs_journal_commit_t *)journal_block;
    c->magic = LEANFS_JOURNAL_COMMIT_MAGIC;
    c->count = n;
    c->sequence = journal_sequence;
    c->checksum = checksum;
    if (device_write(journal_lba(block), BLOCK_DEVICE_PER_LINE, journal_block) != 0) {
        return -1;
    }
    statistics.journal_blocks_written++;

    for (uint32_t k = 0; k < n; k++) {
        journal_map[journal_order[k]].uncommitted = 0;
    }
    journal_uncommitted = 0;
    journal_head = block + 1;
    journal_sequence++;
    journal_last_commit_ms = journal_now_ms();
    statistics.journal_commits++;
    return 0;
}

static int journal_line_in_fs(uint32_t line_no) {
    return journaled && line_no >= journal_fs_first_line && line_no < journal_fs_end_line;
}

/* A whole line into the transaction. A map that is full commits what it has -
   in the middle of an operation, which puts on the disk exactly the state the
   old write-through design would have had there at the same moment - and a
   map that still cannot take it writes the line home, unprotected. */
static int journal_write_line(uint32_t line_no, const uint8_t *data) {
    int e = journal_find(line_no);
    if (e < 0) {
        e = journal_insert(line_no);
        if (e < 0 && journal_commit_locked() == 0 && journal_write_home(0) == 0) {
            e = journal_insert(line_no);
        }
        if (e < 0) {
            return device_write(line_no * BLOCK_DEVICE_PER_LINE, BLOCK_DEVICE_PER_LINE, data);
        }
    }
    k_memcpy(journal_map[e].data, data, LINE_BYTES);
    if (!journal_map[e].uncommitted) {
        journal_map[e].uncommitted = 1;
        if (journal_uncommitted == 0) {
            journal_oldest_uncommitted_ms = journal_now_ms();
        }
        journal_uncommitted++;
    }
    uint32_t s = slot_of(line_no);
    if (tags[s].valid && tags[s].line_no == line_no) {
        k_memcpy(line_pointer[s], data, LINE_BYTES);
    }
    return 0;
}

static void journal_overlay(uint32_t lba, uint32_t count, uint8_t *buffer) {
    if (!journaled || journal_map_used == 0) {
        return;
    }
    uint32_t first_line = lba / BLOCK_DEVICE_PER_LINE;
    uint32_t last_line = (lba + count - 1) / BLOCK_DEVICE_PER_LINE;
    for (uint32_t ln = first_line; ln <= last_line; ln++) {
        int e = journal_find(ln);
        if (e < 0) {
            continue;
        }
        for (uint32_t sector = 0; sector < BLOCK_DEVICE_PER_LINE; sector++) {
            uint32_t at = ln * BLOCK_DEVICE_PER_LINE + sector;
            if (at < lba || at >= lba + count) {
                continue;
            }
            k_memcpy(buffer + (uint64_t)(at - lba) * BLOCK_DEVICE_SECTOR_SIZE,
                     journal_map[e].data + (uint64_t)sector * BLOCK_DEVICE_SECTOR_SIZE,
                     BLOCK_DEVICE_SECTOR_SIZE);
        }
    }
}

int block_device_read(uint32_t lba, uint32_t count, void *buffer) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    int r = read_locked(lba, count, buffer);
    if (r == 0) {
        journal_overlay(lba, count, (uint8_t *)buffer);
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
    return r;
}

int block_device_write(uint32_t lba, uint32_t count, const void *buffer) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    const uint8_t *source = (const uint8_t *)buffer;
    int failed = 0;
    uint32_t i = 0;
    while (i < count) {
        uint32_t line_no = (lba + i) / BLOCK_DEVICE_PER_LINE;
        uint32_t within = (lba + i) % BLOCK_DEVICE_PER_LINE;
        uint32_t run = BLOCK_DEVICE_PER_LINE - within;
        if (run > count - i) {
            run = count - i;
        }
        if (!journal_line_in_fs(line_no)) {
            uint32_t plain = run;
            while (i + plain < count &&
                   !journal_line_in_fs((lba + i + plain) / BLOCK_DEVICE_PER_LINE)) {
                plain++;
            }
            if (write_locked(lba + i, plain, source + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE) != 0) {
                failed = 1;
            }
            i += plain;
            continue;
        }
        const uint8_t *whole = source + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE;
        if (run != BLOCK_DEVICE_PER_LINE) {
            if (read_locked(line_no * BLOCK_DEVICE_PER_LINE, BLOCK_DEVICE_PER_LINE, journal_partial) != 0) {
                failed = 1;
                i += run;
                continue;
            }
            journal_overlay(line_no * BLOCK_DEVICE_PER_LINE, BLOCK_DEVICE_PER_LINE, journal_partial);
            k_memcpy(journal_partial + (uint64_t)within * BLOCK_DEVICE_SECTOR_SIZE, whole,
                     (uint64_t)run * BLOCK_DEVICE_SECTOR_SIZE);
            whole = journal_partial;
        }
        if (journal_write_line(line_no, whole) != 0) {
            failed = 1;
        }
        i += run;
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
    return failed ? -1 : 0;
}

/* Replay: every transaction from the header's start whose descriptors carry
   the next sequence number and whose commit block's checksum matches what is
   between them, written home in order. The first that does not is where the
   power went. A disk without the header has no journal and is not written -
   it may not be this OS's disk at all (M184). */
static int journal_replay(void) {
    if (device_read(journal_lba(0), 1, journal_block) != 0) {
        return -1;
    }
    const leanfs_journal_header_t *h = (const leanfs_journal_header_t *)journal_block;
    if (h->magic != LEANFS_JOURNAL_HEADER_MAGIC || h->version != LEANFS_JOURNAL_VERSION ||
        h->checksum != leanfs_journal_header_checksum(h) || h->journal_blocks != journal_blocks ||
        h->start_block == 0 || h->start_block >= journal_blocks) {
        journal_header_on_disk = 0;
        journal_head = 1;
        journal_sequence = 1;
        return 0;
    }
    journal_header_on_disk = 1;
    uint32_t block = h->start_block;
    uint64_t sequence = h->start_sequence;
    journal_start = block;
    journal_start_sequence = sequence;
    int replayed = 0;
    for (;;) {
        if (block + 2 >= journal_blocks ||
            device_read(journal_lba(block), BLOCK_DEVICE_PER_LINE, journal_block) != 0) {
            break;
        }
        const leanfs_journal_descriptor_t *d = (const leanfs_journal_descriptor_t *)journal_block;
        if (d->magic != LEANFS_JOURNAL_DESCRIPTOR_MAGIC || d->sequence != sequence || d->index != 0 ||
            d->count == 0 || d->count > journal_blocks) {
            break;
        }
        uint32_t n = d->count;
        uint32_t descriptors = leanfs_journal_descriptor_blocks(n);
        if (block + descriptors + n + 1 > journal_blocks) {
            break;
        }
        /* First pass: the checksum, reading descriptors and blocks through
           the staging buffer. Nothing is written home unless all of it is
           there. */
        uint32_t checksum = LEANFS_FNV1A_INIT;
        int intact = 1;
        for (uint32_t done = 0; done < descriptors + n && intact;) {
            uint32_t chunk = descriptors + n - done;
            if (chunk > scratch_lines) {
                chunk = scratch_lines;
            }
            if (device_read(journal_lba(block + done), chunk * BLOCK_DEVICE_PER_LINE, scratch) != 0) {
                intact = 0;
                break;
            }
            for (uint32_t c = 0; c < chunk; c++) {
                uint32_t item = done + c;
                const uint8_t *at = scratch + (uint64_t)c * LINE_BYTES;
                if (item < descriptors) {
                    const leanfs_journal_descriptor_t *dd = (const leanfs_journal_descriptor_t *)at;
                    if (dd->magic != LEANFS_JOURNAL_DESCRIPTOR_MAGIC || dd->sequence != sequence ||
                        dd->index != item || dd->count != n) {
                        intact = 0;
                        break;
                    }
                }
                checksum = leanfs_fnv1a(checksum, at, LINE_BYTES);
            }
            done += chunk;
        }
        if (!intact ||
            device_read(journal_lba(block + descriptors + n), BLOCK_DEVICE_PER_LINE, journal_block) != 0) {
            break;
        }
        const leanfs_journal_commit_t *c = (const leanfs_journal_commit_t *)journal_block;
        if (c->magic != LEANFS_JOURNAL_COMMIT_MAGIC || c->sequence != sequence || c->count != n ||
            c->checksum != checksum) {
            break;
        }
        /* Second pass: home. The targets come from the descriptors, read
           again one at a time, and each block from its place in the run. */
        for (uint32_t item = 0; item < n; item++) {
            if (item % LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR == 0 &&
                device_read(journal_lba(block + item / LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR),
                            BLOCK_DEVICE_PER_LINE, journal_block) != 0) {
                return -1;
            }
            uint32_t target = ((const leanfs_journal_descriptor_t *)journal_block)
                                  ->targets[item % LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR];
            if (target < journal_fs_first_line || target >= journal_fs_end_line) {
                return -1;
            }
            if (device_read(journal_lba(block + descriptors + item), BLOCK_DEVICE_PER_LINE, scratch) != 0 ||
                device_write(target * BLOCK_DEVICE_PER_LINE, BLOCK_DEVICE_PER_LINE, scratch) != 0) {
                return -1;
            }
        }
        replayed++;
        block += descriptors + n + 1;
        sequence++;
    }
    journal_sequence = sequence;
    journal_head = block;
    if (replayed > 0) {
        /* Replay wrote home behind the cache's back. */
        for (uint32_t i = 0; i < cache_lines; i++) {
            if (tags[i].valid && !tags[i].dirty) {
                tags[i].valid = 0;
            }
        }
        statistics.resident = 0;
        statistics.journal_replayed += (uint32_t)replayed;
        journal_head = 1;
        if (journal_write_header(1, journal_sequence) != 0) {
            return -1;
        }
    }
    return replayed;
}

int block_device_journal_attach(uint32_t fs_first_lba, uint32_t fs_sectors, uint32_t journal_first_lba,
                                uint32_t blocks) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    int result = -1;
    if (!journal_map) {
        uint64_t map_pages = ((uint64_t)JOURNAL_MAP_SLOTS * sizeof(journal_entry_t) + 4095) / 4096;
        uint64_t order_pages = ((uint64_t)JOURNAL_MAP_SLOTS * sizeof(uint32_t) + 4095) / 4096;
        uint64_t map_frames = physical_memory_try_alloc_contiguous(map_pages);
        uint64_t order_frames = physical_memory_try_alloc_contiguous(order_pages);
        if (!map_frames || !order_frames) {
            spin_unlock_irqrestore(&block_device_lock, irq);
            return -1;
        }
        journal_map = (journal_entry_t *)(uintptr_t)map_frames;
        journal_order = (uint32_t *)(uintptr_t)order_frames;
        /* Fresh frames are not zeroed; an entry is only trusted to hold a
           pool frame once this has said it holds none. */
        for (uint32_t i = 0; i < JOURNAL_MAP_SLOTS; i++) {
            journal_map[i].line_no = JOURNAL_EMPTY_KEY;
            journal_map[i].data = 0;
        }

        uint64_t affordable = physical_memory_free_frame_count() / 32;
        uint32_t wanted = affordable < JOURNAL_MAP_MAX_ENTRIES ? (uint32_t)affordable : JOURNAL_MAP_MAX_ENTRIES;
        uint64_t pool_pages = ((uint64_t)wanted * sizeof(uint8_t *) + 4095) / 4096;
        uint64_t pool_frames = wanted ? physical_memory_try_alloc_contiguous(pool_pages) : 0;
        if (pool_frames) {
            journal_pool = (uint8_t **)(uintptr_t)pool_frames;
            for (uint32_t k = 0; k < wanted; k++) {
                uint64_t frame = physical_memory_try_alloc_frame();
                if (!frame) {
                    break;
                }
                journal_pool[journal_pool_size++] = (uint8_t *)(uintptr_t)frame;
            }
        }
        journal_pool_free = journal_pool_size;
    }
    for (uint32_t i = 0; i < JOURNAL_MAP_SLOTS; i++) {
        if (journal_map[i].line_no != JOURNAL_EMPTY_KEY && journal_map[i].data) {
            journal_pool[journal_pool_free++] = journal_map[i].data;
        }
        journal_map[i].line_no = JOURNAL_EMPTY_KEY;
        journal_map[i].uncommitted = 0;
        journal_map[i].data = 0;
    }
    journal_map_used = 0;
    journal_uncommitted = 0;
    journal_map_limit = journal_pool_size;
    journal_fs_first_line = fs_first_lba / BLOCK_DEVICE_PER_LINE;
    journal_fs_end_line = (fs_first_lba + fs_sectors) / BLOCK_DEVICE_PER_LINE;
    journal_first_line = journal_first_lba / BLOCK_DEVICE_PER_LINE;
    journal_blocks = blocks;
    journaled = 0;
    if (scratch && blocks >= 4 && journal_pool_size >= 64) {
        result = journal_replay();
        if (result >= 0) {
            journaled = 1;
            journal_last_commit_ms = journal_now_ms();
        }
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
    return result;
}

void block_device_journal_forget(void) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    journaled = 0;
    journal_header_on_disk = 0;
    if (journal_map) {
        for (uint32_t i = 0; i < JOURNAL_MAP_SLOTS; i++) {
            if (journal_map[i].line_no != JOURNAL_EMPTY_KEY && journal_map[i].data) {
                journal_pool[journal_pool_free++] = journal_map[i].data;
            }
            journal_map[i].line_no = JOURNAL_EMPTY_KEY;
            journal_map[i].data = 0;
        }
    }
    journal_map_used = 0;
    journal_uncommitted = 0;
    spin_unlock_irqrestore(&block_device_lock, irq);
}

/* A fresh filesystem has no history to replay: the header is rewritten so
   nothing of an older one on the same disk ever is. */
int block_device_journal_reset(void) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    int r = -1;
    if (journaled) {
        journal_head = 1;
        r = journal_write_header(1, journal_sequence);
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
    return r;
}

int block_device_commit(void) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    int r = journal_commit_locked();
    spin_unlock_irqrestore(&block_device_lock, irq);
    return r;
}

int block_device_checkpoint(void) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    int r = 0;
    if (journaled) {
        r = journal_commit_locked();
        if (r == 0 && (journal_map_used > 0 || !journal_header_on_disk || journal_head != 1)) {
            r = journal_write_home(0);
        }
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
    return r;
}

void block_device_commit_if_due(void) {
    uint64_t irq = spin_lock_irqsave(&block_device_lock);
    if (journaled) {
        uint64_t now = journal_now_ms();
        if (journal_uncommitted > 0 &&
            (now >= journal_oldest_uncommitted_ms + JOURNAL_COMMIT_DELAY_MS ||
             journal_uncommitted >= journal_map_limit / 2)) {
            journal_commit_locked();
        }
        if (journal_uncommitted == 0 && journal_map_used > 0 &&
            (now >= journal_last_commit_ms + JOURNAL_IDLE_CHECKPOINT_MS ||
             journal_head > journal_blocks - journal_blocks / 4)) {
            journal_write_home(0);
        }
    }
    spin_unlock_irqrestore(&block_device_lock, irq);
}

/* The tail of the journal, for a benchmark that needs blocks nobody else is
   using: commits start from the front after a checkpoint and would have to
   fill nearly all of it to reach here. */
uint32_t block_device_journal_scratch_lba(uint32_t blocks) {
    if (block_device_checkpoint() != 0 || !journaled || blocks + 64 >= journal_blocks) {
        return 0;
    }
    return journal_lba(journal_blocks - blocks);
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
