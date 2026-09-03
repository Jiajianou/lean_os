#include "blk.h"

#include "drivers/ata.h"
#include "drivers/klog.h"
#include "drivers/virtio_blk.h"
#include "lib/libk.h"
#include "lib/spinlock.h"
#include "drivers/pit.h" /* M104: the flush deadline is measured in ticks */
#include "mm/pmm.h"

/* One cache line is eight sectors - 4 KiB, a page, and the unit leanfs's
 * own block size already is. Caching at sector granularity would triple
 * the tag overhead for the same bytes and would not match a single access
 * pattern in the filesystem above it. */
#define BLK_PER_LINE 8
#define LINE_BYTES   (BLK_PER_LINE * BLK_SECTOR_SIZE)

/* 2048 lines is 8 MiB of cache.
 *
 * The number is chosen against what it has to hold rather than against
 * what the machine has. leanfs's inode table is 2048 sectors (M81) and
 * its block bitmap is 16 more; a directory scan walks those repeatedly,
 * and everything else is data blocks that are read once. 8 MiB holds the
 * whole metadata region several times over with room for the working set
 * of a tree walk, on a machine M90 established has gigabytes. It is a
 * fixed allocation rather than a fraction of memory because a cache whose
 * size depends on the machine is a cache whose measurements do not
 * compare between two boots. */
#define CACHE_LINES 2048

typedef struct {
    uint32_t line_no;  /* lba / BLK_PER_LINE */
    uint8_t  valid;
    /* ---- M104: writeback ---------------------------------------------
     *
     * `dirty` means this line holds bytes the device does not. M92 built
     * the cache write-through and argued for it at length: "a cache that
     * reorders writes turns 'write ordering plus a mount check' into
     * neither, and M71's whole argument for not having a journal rests
     * on the first half being true."
     *
     * That argument is answered rather than overruled. Ordering is
     * preserved by a BARRIER: leanfs calls blk_barrier() at exactly the
     * points where its ordering matters - before it writes the metadata
     * that points at data it has just written - and a barrier flushes
     * every dirty line. So the disk still sees data before the metadata
     * that names it, which is the whole of M71's guarantee; what changes
     * is that a burst of data writes between two barriers becomes one
     * device request instead of many.
     *
     * `dirty_tick` is when it became dirty, for the deadline. A line
     * that nothing barriers or syncs still reaches the disk within
     * BLK_FLUSH_DEADLINE_MS, so an idle machine's cache does not hold
     * unwritten bytes indefinitely. */
    uint8_t  dirty;
    uint64_t dirty_tick;
} cache_tag_t;

/* How long a dirty line may wait for a barrier that never comes.
 *
 * Five seconds is the number every Unix has used for this since the
 * 1970s `update` daemon, and it is not chosen by measurement here
 * because what it trades is not performance against performance: it is
 * "how much work an unclean shutdown can lose" against "how many device
 * writes a workload costs", and the second is already bounded by the
 * barriers. Said out loud rather than tuned. */
#define BLK_FLUSH_DEADLINE_MS 5000

static cache_tag_t tags[CACHE_LINES];
/* One frame per line, and deliberately NOT one contiguous run.
 *
 * The first version of this allocated 2048 frames and required them to
 * come back consecutive, on the theory that a fresh allocator hands out
 * consecutive frames. That theory is false here and the reason is worth
 * recording: blk_init runs late in boot, after every self-test that
 * allocates and frees - M90's own probe frees a frame at 4 GiB and pulls
 * the allocator's search hint back with it - so the free list is holed by
 * the time this asks. A line is exactly one page, so there is no reason
 * to need contiguity at all, and an array of pointers costs 16 KiB.
 *
 * pmm_alloc_frame rather than pmm_alloc_contiguous for the same reason:
 * the cache is never handed to a device, so taking megabytes out of the
 * one region a 32-bit DMA engine can reach (M90's PMM_DMA_LIMIT) would be
 * taking it from the one place a driver cannot do without. */
static uint8_t *line_ptr[CACHE_LINES];
static uint32_t cache_lines; /* how many of them actually exist - see blk_init */
/* M104: when the oldest currently-dirty line became dirty. One number so
 * the deadline check is a compare rather than a scan - see
 * flush_if_overdue_locked, which learned that the hard way. */
static uint64_t oldest_dirty_ms;

static int have_virtio;
static blk_stats_t stats;

/* One lock over the whole cache. leanfs already serialises itself above
 * this, and every access here is short; a finer-grained scheme would be
 * complexity with no contention to relieve. Deliberately not a leaf lock
 * in the pmm sense - blk_init allocates, but nothing after it does. */
static spinlock_t blk_lock;

/* ---- Q16: a disk that fails, on purpose -------------------------------
 *
 * Under QEMU the disk does not fail, which is why six halts in two
 * drivers survived ninety milestones without one of them ever running.
 * A fault this layer injects is the instrument that makes the *other*
 * side of every one of those - the error path - a thing that has
 * executed.
 *
 * It lives here rather than in the drivers because here is the one place
 * both of them meet, so a test written against it grades whichever
 * driver the machine actually has. `QEMU_DISK=ide` and virtio are
 * supported paths on the same terms, and a fault-injection facility that
 * only worked on one of them would be a test of the backend rather than
 * of the filesystem above it.
 *
 * Deliberately a counter rather than a probability: "the tenth write
 * from now" is reproducible and "one write in ten" is not, and a
 * reproducible failure is the whole difference between a test and an
 * anecdote. QEMU's own `blkdebug` is the other half - see
 * tools/run-qemu.sh - and covers the case this cannot, which is a driver
 * that is lied to by real hardware rather than by the layer above it. */
static int64_t fault_reads_after = -1;  /* -1: never */
static int64_t fault_writes_after = -1;
static uint64_t reads_issued;
static uint64_t writes_issued;
static uint64_t io_errors;

void blk_fault_inject(int64_t fail_reads_after, int64_t fail_writes_after) {
    fault_reads_after = fail_reads_after;
    fault_writes_after = fail_writes_after;
    reads_issued = 0;
    writes_issued = 0;
}

uint64_t blk_error_count(void) {
    return io_errors;
}

static int device_read(uint32_t lba, uint32_t count, void *buf) {
    stats.device_reads += count;
    if (fault_reads_after >= 0 && (int64_t)reads_issued++ >= fault_reads_after) {
        io_errors++;
        return -1;
    }
    if (have_virtio) {
        if (virtio_blk_read(lba, count, buf) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    /* ata_read_sectors takes a uint8_t count, so a run longer than 255
     * has to be split - the same loop leanfs used to carry itself. */
    uint8_t *dst = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        if (ata_read_sectors(lba, (uint8_t)n, dst) != 0) {
            io_errors++;
            return -1;
        }
        dst += n * BLK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static int device_write(uint32_t lba, uint32_t count, const void *buf) {
    stats.device_writes += count;
    if (fault_writes_after >= 0 && (int64_t)writes_issued++ >= fault_writes_after) {
        io_errors++;
        return -1;
    }
    if (have_virtio) {
        if (virtio_blk_write(lba, count, buf) != 0) {
            io_errors++;
            return -1;
        }
        return 0;
    }
    const uint8_t *src = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        if (ata_write_sectors(lba, (uint8_t)n, src) != 0) {
            io_errors++;
            return -1;
        }
        src += n * BLK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

void blk_init(void) {
    have_virtio = virtio_blk_init();

    /* Sized against what the machine has as well as against what the
     * filesystem needs. CACHE_LINES is the ceiling; a sixteenth of free
     * memory is the floor under it, so the 128 MiB machine M90's second
     * run proves this kernel still boots on does not hand an eighth of
     * itself to a cache. The number that came out is logged, because a
     * measurement taken against a cache of unknown size is not a
     * measurement. */
    cache_lines = CACHE_LINES;
    uint64_t affordable = pmm_free_frame_count() / 16;
    if (affordable < cache_lines) {
        cache_lines = (uint32_t)affordable;
    }
    if (cache_lines < 64) {
        cache_lines = 64; /* below this it is not a cache, it is a buffer */
    }

    /* M102: a cache that gets fewer lines rather than a machine that
     * halts. This runs at boot on a machine that has just counted its
     * memory and asks for a sixteenth of it, so a failure here should be
     * impossible - but "should be impossible" is what pmm_alloc_frame's
     * panic was asserting, and a cache is the one structure on this
     * machine that is by definition optional. Whatever it gets, it uses;
     * the number is logged either way, so a short cache is visible rather
     * than silent. */
    uint32_t got = 0;
    for (uint32_t i = 0; i < cache_lines; i++) {
        uint64_t frame = pmm_try_alloc_frame();
        if (!frame) {
            break;
        }
        line_ptr[i] = (uint8_t *)(uintptr_t)frame;
        tags[i].valid = 0;
        got++;
    }
    cache_lines = got;
    stats.capacity = cache_lines;

    klog_puts("[blk] ");
    klog_puts(blk_backend_name());
    klog_puts(", ");
    klog_put_dec((cache_lines * LINE_BYTES) / 1024);
    klog_puts(" KiB write-through cache in ");
    klog_put_dec(cache_lines);
    klog_puts(" lines of ");
    klog_put_dec(LINE_BYTES);
    klog_puts(" bytes\n");
}

const char *blk_backend_name(void) {
    return have_virtio ? "virtio-blk" : "ata-pio";
}

/* Direct-mapped: a line's home is its own number modulo the cache size.
 *
 * Deliberately not set-associative, and the argument is the same one M87
 * made for a three-entry mount table searched by prefix: an index that is
 * one modulo is faster than anything cleverer and easier to be sure of.
 * The pathological case - two hot lines whose numbers differ by exactly
 * cache_lines - costs a miss each time, and the filesystem's hot region
 * (the inode table) is contiguous, which is the shape a direct-mapped
 * cache handles best rather than worst. If a measurement ever shows
 * thrashing, associativity is the answer and this is where it goes. */
/* Forward declarations: the write path defines these and the read path
 * uses them, and the read path is first in this file because that is the
 * order they were written in. */
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

/* ---- One request, not one per line ------------------------------------
 *
 * The obvious cache is per-line: for each line the caller touches, fetch
 * it if absent, then copy out. That is correct and it throws away most of
 * what the new driver bought. leanfs reads its 2048-sector inode table in
 * one call; a per-line fetch turns that into 256 separate device requests
 * of eight sectors each, and on a device where the cost is per *request*
 * rather than per byte - which is exactly what changes when PIO becomes
 * DMA - that is 256 round trips to avoid one.
 *
 * So the decision is made once for the whole range. If every line it
 * touches is already resident, it is served entirely from memory. If any
 * is missing, the whole range is read in a single request straight into
 * the caller's buffer, and the cache is then populated from what came
 * back - for the lines the range covers completely, since a partly
 * covered line at either end cannot be filled from this buffer alone.
 *
 * Populating from the caller's buffer rather than re-reading is safe for
 * one reason and it is the same reason eviction is free: the cache is
 * write-through, so what is on the disk and what is in a resident line
 * are the same bytes by construction. There is no version of this where
 * the copy could be stale. */
/* ---- M104: readahead --------------------------------------------------
 *
 * How many extra lines to pull in past the end of a sequential read.
 * `0` disables it, which is what the measurement below compares against.
 *
 * Sequential is detected by the simplest rule that is true of the
 * workload: this read starts exactly where the last one ended. A
 * compiler walking a header tree and a `treewalk` hashing a source tree
 * both produce that pattern; a random-access workload does not, and gets
 * no readahead rather than a wrong guess about one.
 *
 * The eviction cost is real and is why this is a small number: a line
 * fetched speculatively takes a slot from a line somebody asked for, and
 * a direct-mapped cache has no second chance. Eight lines is 32 KiB - a
 * whole leanfs indirect block's worth of data - and is the number the
 * measurement in the [m104] self-test compares against zero. */
/* ---- M104: measured, and the measurement said zero --------------------
 *
 * The default is 0 - readahead OFF - and that is the result of the
 * measurement rather than a decision not to build it. The [m104]
 * self-test reads a megabyte sequentially both ways:
 *
 *     without readahead   4899 us
 *     with 8 lines ahead  9075 us
 *
 * It is not marginally worse, it is nearly twice as slow, and the reason
 * is visible once stated: leanfs already reads in 64 KiB requests, which
 * is sixteen lines. A request is what costs on this device (M92 measured
 * exactly that when PIO became DMA), so adding eight speculative lines
 * as a SECOND request roughly doubles the request count to fetch data
 * the next call would have asked for in its own large request anyway.
 * Readahead helps a device where the cost is per byte and a filesystem
 * that reads in small pieces. This is neither.
 *
 * The code stays, and blk_set_readahead is what the self-test uses to
 * take the measurement each boot - so the day either of those facts
 * changes, the number is already being produced rather than having to be
 * argued for again. M69's rule, applied to a feature the milestone asked
 * for by name. */
static uint32_t readahead_lines = 0;
static uint32_t last_read_end;   /* the lba just past the previous read */

void blk_set_readahead(uint32_t lines) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    readahead_lines = lines;
    spin_unlock_irqrestore(&blk_lock, irq);
}

int blk_read(uint32_t lba, uint32_t count, void *buf) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    flush_if_overdue_locked();
    int sequential = (lba == last_read_end);
    last_read_end = lba + count;
    stats.reads++;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t first_line = lba / BLK_PER_LINE;
    uint32_t last_line = (lba + count - 1) / BLK_PER_LINE;

    if (all_lines_resident(first_line, last_line)) {
        stats.hits++;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = slot_of((lba + i) / BLK_PER_LINE);
            uint32_t within = (lba + i) % BLK_PER_LINE;
            k_memcpy(dst + (uint64_t)i * BLK_SECTOR_SIZE,
                     line_ptr[s] + (uint64_t)within * BLK_SECTOR_SIZE,
                     BLK_SECTOR_SIZE);
        }
        spin_unlock_irqrestore(&blk_lock, irq);
        return 0;
    }

    if (device_read(lba, count, dst) != 0) {
        /* Q16: zeroed rather than left as it was found.
         *
         * A caller that ignores the return value - and there will be one
         * eventually, because this call has been `void` since M92 -
         * would otherwise parse whatever was already in its buffer as
         * filesystem metadata. Zeros are not a valid superblock, a valid
         * inode or a valid directory entry, so a caller that ignores the
         * error fails at the next check instead of following a pointer
         * made of somebody else's stack. */
        k_memset(dst, 0, (uint64_t)count * BLK_SECTOR_SIZE);
        spin_unlock_irqrestore(&blk_lock, irq);
        return -1;
    }

    for (uint32_t ln = first_line; ln <= last_line; ln++) {
        uint32_t line_lba = ln * BLK_PER_LINE;
        if (line_lba < lba || line_lba + BLK_PER_LINE > lba + count) {
            continue; /* only partly covered by this request */
        }
        uint32_t s = slot_of(ln);
        if (!tags[s].valid) {
            stats.resident++;
        }
        if (tags[s].dirty && tags[s].line_no != ln) {
            flush_line(s); /* M104: never evict unwritten bytes */
        }
        k_memcpy(line_ptr[s], dst + (uint64_t)(line_lba - lba) * BLK_SECTOR_SIZE, LINE_BYTES);
        tags[s].line_no = ln;
        tags[s].valid = 1;
        tags[s].dirty = 0;
    }

    /* M104: and the lines after it, if this looked sequential.
     *
     * One extra device request, after the caller's data is already in
     * their buffer - so a readahead that turns out to be wasted costs a
     * request and nothing the caller waits for twice. Read into the
     * cache lines directly rather than through a staging buffer, which
     * is possible because a line is exactly a page and they need not be
     * adjacent. */
    if (sequential && readahead_lines > 0) {
        for (uint32_t k = 0; k < readahead_lines; k++) {
            uint32_t ln = last_line + 1 + k;
            uint32_t s = slot_of(ln);
            if (tags[s].valid && tags[s].line_no == ln) {
                continue; /* already here */
            }
            if (tags[s].dirty) {
                break; /* not worth evicting unwritten bytes to guess */
            }
            if (device_read(ln * BLK_PER_LINE, BLK_PER_LINE, line_ptr[s]) != 0) {
                /* A readahead is a guess. A guess that fails is not an
                 * error the caller asked about - its own data is already
                 * in its buffer - so this stops guessing and says
                 * nothing, which is the only behaviour that keeps
                 * readahead from turning a healthy read into a failed
                 * one. */
                break;
            }
            if (!tags[s].valid) {
                stats.resident++;
            }
            tags[s].line_no = ln;
            tags[s].valid = 1;
            tags[s].dirty = 0;
            stats.readaheads++;
        }
    }
    spin_unlock_irqrestore(&blk_lock, irq);
    return 0;
}

/* Writes one dirty line back and clears the flag. Caller holds the
 * lock.
 *
 * Q16: a write that fails leaves the line DIRTY, which is the whole of
 * the recovery story and is worth stating. The bytes are still the only
 * copy that exists, so the next barrier tries again and an eviction
 * refuses to discard them; a flush_line that cleared the flag on failure
 * would silently drop a filesystem's writes, which is the failure mode
 * that only appears in the runs nobody is watching. */
static int flush_line(uint32_t s) {
    if (!tags[s].valid || !tags[s].dirty) {
        return 0;
    }
    if (device_write(tags[s].line_no * BLK_PER_LINE, BLK_PER_LINE, line_ptr[s]) != 0) {
        return -1;
    }
    tags[s].dirty = 0;
    stats.writebacks++;
    return 0;
}

static int flush_all_locked(void) {
    if (stats.dirty == 0) {
        return 0; /* the common case, and it must cost nothing */
    }
    /* Q16: every line is attempted even after one fails, and the count
     * of those still dirty replaces the unconditional `stats.dirty = 0`.
     *
     * Stopping at the first failure would leave lines that would have
     * been written unwritten, on the theory that a disk which failed
     * once will fail again - which is a guess, and the cheap version of
     * being wrong about it is losing data the device would have
     * accepted. */
    uint32_t still_dirty = 0;
    for (uint32_t i = 0; i < cache_lines; i++) {
        if (flush_line(i) != 0) {
            still_dirty++;
        }
    }
    stats.dirty = still_dirty;
    return still_dirty == 0 ? 0 : -1;
}

/* M104: the deadline, checked on the way through rather than from a
 * timer.
 *
 * A kernel thread that woke every five seconds to look at a cache that
 * is usually clean would be a thread and a wake for nothing. Every path
 * that touches the cache passes through here, and a machine doing no I/O
 * at all has no dirty lines to worry about - which is the case a timer
 * would exist to handle and is exactly the case where it is not needed.
 * The cost of being late is bounded by the deadline plus the gap to the
 * next cache access, and the only way to have a long gap is to be doing
 * nothing. */
static void flush_if_overdue_locked(void) {
    if (stats.dirty == 0) {
        return;
    }
    /* One compare, not a scan.
     *
     * The first version of this walked all 2048 lines looking for an
     * overdue one - on EVERY read and write - and the cold-read
     * benchmark went from 4.9 ms per megabyte to 9.2. A deadline check
     * that costs more than the I/O it is deciding about is a deadline
     * check that has to be one number, and the oldest dirty line is the
     * only one the answer depends on. */
    uint64_t now = pit_get_ticks() * (1000 / PIT_HZ);
    if (now >= oldest_dirty_ms + BLK_FLUSH_DEADLINE_MS) {
        flush_all_locked();
    }
}

int blk_write(uint32_t lba, uint32_t count, const void *buf) {
    int failed = 0;
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    flush_if_overdue_locked();
    const uint8_t *src = (const uint8_t *)buf;

    /* ---- M104: which writes are deferred, and which are not -----------
     *
     * A write that covers a WHOLE cache line can be absorbed: the line
     * becomes the truth, the device is told later, and a second write to
     * the same line before the next barrier costs nothing at all. That
     * is the case an unpack and a compiler both generate, because both
     * write whole blocks.
     *
     * A partial line still goes straight to the device. Absorbing one
     * would mean either reading the rest of the line in first - a
     * read-modify-write of 4 KiB to change 512 bytes, which is exactly
     * the traffic M92's note said must not be generated - or tracking
     * which sectors within a line are dirty, which is a bitmap per line
     * to save a case leanfs does not produce: it speaks in 4 KiB blocks
     * and a block is a line.
     */
    uint32_t i = 0;
    while (i < count) {
        uint32_t line_no = (lba + i) / BLK_PER_LINE;
        uint32_t within = (lba + i) % BLK_PER_LINE;
        uint32_t remaining = count - i;
        int whole_line = (within == 0 && remaining >= BLK_PER_LINE);

        if (whole_line) {
            uint32_t s = slot_of(line_no);
            /* Claim the slot whether or not it was this line: the write
             * supplies every byte, so nothing has to be read first, and
             * evicting somebody else's clean line to absorb a write is
             * the trade this cache exists to make. A dirty line being
             * evicted is written back first, or its bytes would be
             * lost. */
            if (tags[s].valid && tags[s].dirty && tags[s].line_no != line_no) {
                if (flush_line(s) != 0) {
                    /* Q16: the eviction failed, so the bytes already in
                     * this slot are still the only copy. Overwriting
                     * them to absorb the new write would lose them
                     * silently, which is the one outcome this cache must
                     * never produce - so the new write goes straight to
                     * the device instead, and both stay accounted for. */
                    if (device_write(lba + i, BLK_PER_LINE,
                                     src + (uint64_t)i * BLK_SECTOR_SIZE) != 0) {
                        failed = 1;
                    }
                    i += BLK_PER_LINE;
                    continue;
                }
            }
            if (!tags[s].valid) {
                stats.resident++;
            }
            k_memcpy(line_ptr[s], src + (uint64_t)i * BLK_SECTOR_SIZE, LINE_BYTES);
            if (!tags[s].dirty) {
                if (stats.dirty == 0) {
                    oldest_dirty_ms = pit_get_ticks() * (1000 / PIT_HZ);
                }
                stats.dirty++;
                tags[s].dirty_tick = oldest_dirty_ms;
            }
            tags[s].valid = 1;
            tags[s].line_no = line_no;
            tags[s].dirty = 1;
            i += BLK_PER_LINE;
            continue;
        }

        /* A partial line: through to the device, and into the cache if
         * the line happens to be resident so a later read does not see
         * the old bytes. */
        if (device_write(lba + i, 1, src + (uint64_t)i * BLK_SECTOR_SIZE) != 0) {
            failed = 1;
        }
        uint32_t s = slot_of(line_no);
        if (tags[s].valid && tags[s].line_no == line_no) {
            k_memcpy(line_ptr[s] + (uint64_t)within * BLK_SECTOR_SIZE,
                     src + (uint64_t)i * BLK_SECTOR_SIZE, BLK_SECTOR_SIZE);
        }
        i++;
    }
    spin_unlock_irqrestore(&blk_lock, irq);
    /* A whole-line write that was absorbed returns success, and that is
     * not a lie: the cache has accepted responsibility for those bytes
     * and the barrier before the metadata that names them is where the
     * device gets its say. M71's ordering guarantee is a statement about
     * barriers, not about individual writes, and this is the same
     * contract M104 already established - Q16 only makes its failures
     * visible. */
    return failed ? -1 : 0;
}

/* M104: everything dirty, to the device, now.
 *
 * This is both `blk_flush` and the barrier leanfs calls before writing
 * metadata - they are the same operation and giving them two names would
 * suggest they could differ. See cache_tag_t's note for why a barrier is
 * what preserves M71's ordering guarantee across a writeback cache. */
int blk_flush(void) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    int r = flush_all_locked();
    spin_unlock_irqrestore(&blk_lock, irq);
    return r;
}

void blk_stats(blk_stats_t *out) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    *out = stats;
    spin_unlock_irqrestore(&blk_lock, irq);
}

void blk_cache_drop(void) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    /* M104: dirty lines are WRITTEN, not discarded. Dropping the cache
     * is a measurement tool - it is how the disk benchmark gets a cold
     * read - and a measurement tool that silently lost a filesystem's
     * writes would be the worst kind of bug: one that only appears in
     * the runs nobody is looking at. */
    flush_all_locked();
    for (uint32_t i = 0; i < cache_lines; i++) {
        tags[i].valid = 0;
        tags[i].dirty = 0;
    }
    stats.resident = 0;
    stats.dirty = 0;
    spin_unlock_irqrestore(&blk_lock, irq);
}
