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

void blk_read(uint32_t lba, uint32_t count, void *buf);
void blk_write(uint32_t lba, uint32_t count, const void *buf);

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
void blk_flush(void);

/* M104: how many lines to fetch past the end of a sequential read. 0
 * disables it, which is what the milestone's own measurement compares
 * against - see blk.c for how "sequential" is decided and why the
 * number is small. */
void blk_set_readahead(uint32_t lines);

void blk_cache_drop(void);
