/* kernel/drivers/blk.h - M92
 *
 * The block layer leanfs talks to, and the cache in front of it.
 *
 * Until M92 kernel/fs/leanfs.c called ata_read_sectors/ata_write_sectors
 * directly, twenty-four times, and that was exactly right while there was
 * one disk driver and one way to reach it. Two things made it stop being
 * right at once: there is now a second driver (drivers/virtio_blk.h), and
 * every read went to the device however many times the same sector was
 * asked for. A filesystem that is about to hold a source tree re-reads
 * its inode table and its directory blocks constantly.
 *
 * ---- The cache is write-through, and that is a decision --------------
 *
 * M71 bought "files worth trusting" with write ordering plus a mount
 * check, and its own note says a journal becomes worth it when there are
 * multiple writers or when a full scan gets slow. A write-BACK cache is
 * the thing that would quietly make the first half of that false: the
 * whole guarantee is that the data block reaches the disk before the
 * metadata pointing at it, and a cache free to reorder or defer writes
 * turns "write ordering plus a mount check" into a mount check.
 *
 * So writes go to the device in the order they were issued, and are
 * *also* placed in the cache so the next read of the same block is free.
 * That keeps M71 exactly as true as it was and still collects most of the
 * win, because the traffic a filesystem generates is overwhelmingly
 * reads: an unpack writes each block once and reads its inode table,
 * bitmap and directory blocks over and over.
 *
 * Writeback is not being deferred out of caution - it is being declined
 * until there is a journal, which is the same conclusion from the other
 * direction, and M93's own bullet is where that gets re-measured.
 */
#pragma once

#include <stdint.h>

#define BLK_SECTOR_SIZE 512

/* Probes for a virtio block device and falls back to ATA PIO. Must be
 * called before leanfs mounts anything. */
void blk_init(void);

/* Which backend is live - "virtio-blk" or "ata-pio". For the boot log and
 * for the self-test that reports both numbers. */
const char *blk_backend_name(void);

/* ---- Q16: these return now, and every one of them can fail ------------
 *
 * 0 on success, -1 if the device refused. Both were `void` from M92
 * until Q16, which was accurate about a machine that had only ever run
 * under an emulator whose disk does not fail: the drivers underneath
 * halted, so there was nothing to report.
 *
 * A failed READ leaves the caller's buffer ZEROED rather than as it was
 * found. That is a decision about a caller that ignores the return
 * value, and there will be one: zeros are not a valid superblock, inode
 * or directory entry, so such a caller fails its next check instead of
 * following a pointer made of whatever was in its buffer.
 *
 * A WRITE that the cache absorbed returns success and means it - the
 * cache has taken responsibility for those bytes, and blk_flush is where
 * the device gets its say. That is M104's contract unchanged; Q16 only
 * makes its failures visible. A line that fails to write back stays
 * DIRTY, so the next barrier retries it and an eviction will not discard
 * it. */
int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);

/* Q16: a disk that fails on purpose, and how many times it has.
 *
 * `blk_fault_inject(r, w)` makes the r'th device read and the w'th
 * device write from now on fail, and every one after it; -1 for either
 * means "never". Both counters restart on every call, and
 * `blk_fault_inject(-1, -1)` puts the disk back.
 *
 * A counter rather than a probability, deliberately: "the tenth write
 * from now" is reproducible and "one write in ten" is not, and a
 * reproducible failure is the difference between a test and an anecdote.
 * It lives at this layer because this is where the two drivers meet, so
 * one test grades whichever backend the machine actually has -
 * `QEMU_DISK=ide` and virtio are supported paths on the same terms.
 *
 * blk_error_count is the number a machine with an occasionally-failing
 * disk reports and one with a healthy disk does not, which is what makes
 * the two distinguishable from outside. */
void blk_fault_inject(int64_t fail_reads_after, int64_t fail_writes_after);
uint64_t blk_error_count(void);

/* M92: the cache's own numbers, so the milestone can report a ratio
 * rather than an adjective. Hits and misses since boot, and the count of
 * blocks currently held. */
typedef struct {
    uint64_t reads;      /* blk_read calls */
    uint64_t hits;       /* ...of which were served without touching the device */
    uint64_t device_reads;  /* sector-granularity reads issued to the driver */
    uint64_t device_writes; /* ...and writes */
    uint32_t resident;   /* cache blocks currently holding data */
    uint32_t capacity;   /* cache blocks in total */
    /* M104 */
    uint32_t dirty;      /* lines holding bytes the device does not have */
    uint64_t writebacks; /* lines written back by a barrier, a deadline or an eviction */
    uint64_t readaheads; /* lines fetched speculatively past a sequential read */
} blk_stats_t;

void blk_stats(blk_stats_t *out);

/* M92: empties the cache without touching the device - every block in it
 * is identical to what is on disk, because this cache is write-through.
 * Exists so a self-test can measure a COLD read, which is the number that
 * says what the device costs; a warm one says what the cache costs and
 * the pair is the measurement. */
/* ---- M104: write everything dirty to the device, now -------------------
 *
 * Also the barrier leanfs uses to keep M71's write ordering across a
 * writeback cache: it is called before the metadata that points at data
 * is written, so the disk sees them in that order however the cache
 * absorbed them. They are one function because they are one operation,
 * and two names would suggest they could differ.
 *
 * Called by vfs_sync, SYS_fsync, SYS_sync, the shutdown path, and by
 * leanfs's own save_meta and save_superblock. */
int blk_flush(void);

/* M104: how many lines to fetch past the end of a sequential read. 0
 * disables it, which is what the milestone's own measurement compares
 * against - see blk.c for how "sequential" is decided and why the
 * number is small. */
void blk_set_readahead(uint32_t lines);

void blk_cache_drop(void);
