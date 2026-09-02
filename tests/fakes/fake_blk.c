/* tests/fakes/fake_blk.c - Q3
 *
 * A disk made of host memory, and the counters that make it an
 * instrument rather than a stand-in.
 *
 * kernel/fs/leanfs.c is 2,336 lines sitting behind five functions in
 * kernel/drivers/blk.h. That is the seam, and it is the reason a
 * filesystem written for a real disk can be exercised in a fraction of a
 * second without one.
 *
 * The counters matter as much as the storage. A test that only checks
 * "the bytes came back" can regress an outcome; one that can also say
 * "and it cost four device reads" can regress an *algorithm*, which is
 * the thing nothing in this project has been able to do. The block cache
 * M92 added is the obvious beneficiary: whether it works is a question
 * about counts.
 *
 * fake_blk_fail_writes_after() is the other half - a disk that stops
 * accepting writes partway through an operation, which is how the
 * ordering guarantee M71 bought gets tested rather than asserted. */
#include "drivers/blk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *msg);

static uint8_t *disk;
static uint32_t disk_sectors;
static uint64_t reads;
static uint64_t writes;
static int64_t fail_writes_after = -1;

void fake_blk_reset(uint32_t sectors);
void fake_blk_free(void);
void fake_blk_reset_counters(void);
uint64_t fake_blk_reads(void);
uint64_t fake_blk_writes(void);
uint8_t *fake_blk_sector(uint32_t lba);
uint32_t fake_blk_sector_count(void);
void fake_blk_fail_writes_after(int64_t n);

void fake_blk_reset(uint32_t sectors) {
    /* Q4: the allocation is reused when the size has not changed.
     *
     * This was a free/calloc pair, which is correct and was costing the
     * leanfs fuzzer most of its throughput - a 21 MB calloc per input, at
     * ninety-four inputs a second. A fuzzer that slow is a fuzzer that
     * finds nothing. Same observable behaviour: the disk is still all
     * zeroes when this returns. */
    if (disk && disk_sectors == sectors) {
        memset(disk, 0, (size_t)sectors * BLK_SECTOR_SIZE);
    } else {
        free(disk);
        disk = (uint8_t *)calloc(sectors, BLK_SECTOR_SIZE);
        if (!disk) {
            panic("fake_blk: could not allocate the RAM disk");
        }
        disk_sectors = sectors;
    }
    reads = writes = 0;
    fail_writes_after = -1;
}

void fake_blk_free(void) {
    free(disk);
    disk = NULL;
    disk_sectors = 0;
}

void fake_blk_reset_counters(void) { reads = writes = 0; }
uint64_t fake_blk_reads(void) { return reads; }
uint64_t fake_blk_writes(void) { return writes; }
uint32_t fake_blk_sector_count(void) { return disk_sectors; }
void fake_blk_fail_writes_after(int64_t n) { fail_writes_after = n; }

uint8_t *fake_blk_sector(uint32_t lba) {
    if (!disk || lba >= disk_sectors) {
        panic("fake_blk_sector: out of range");
    }
    return disk + (size_t)lba * BLK_SECTOR_SIZE;
}

/* Out-of-range access panics rather than being clamped or ignored. On the
 * machine an LBA past the end of the disk is a driver-level error that
 * this filesystem is supposed to never generate; a test that silently
 * tolerated one would be hiding exactly the bug worth finding. */
static void range_check(uint32_t lba, uint32_t count, const char *what) {
    if (!disk) {
        panic("fake_blk: access before fake_blk_reset");
    }
    if (count == 0 || lba > disk_sectors || count > disk_sectors - lba) {
        panic(what);
    }
}

void blk_read(uint32_t lba, uint32_t count, void *buf) {
    range_check(lba, count, "fake_blk: read past the end of the disk");
    memcpy(buf, disk + (size_t)lba * BLK_SECTOR_SIZE, (size_t)count * BLK_SECTOR_SIZE);
    reads += count;
}

void blk_write(uint32_t lba, uint32_t count, const void *buf) {
    range_check(lba, count, "fake_blk: write past the end of the disk");
    if (fail_writes_after >= 0 && (int64_t)writes >= fail_writes_after) {
        /* Counted but not performed - a disk that has stopped taking
         * writes and has not said so, which is the failure a write
         * ordering guarantee is supposed to survive. */
        writes += count;
        return;
    }
    memcpy(disk + (size_t)lba * BLK_SECTOR_SIZE, buf, (size_t)count * BLK_SECTOR_SIZE);
    writes += count;
}

void blk_init(void) {}
const char *blk_backend_name(void) { return "fake-ram"; }
void blk_cache_drop(void) {}

void blk_stats(blk_stats_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->reads = reads;
    out->device_reads = reads;
    out->device_writes = writes;
}

/* M104: the host tier's disk has no cache, so a flush is a no-op - and
 * that is the right fake rather than a stub, because what leanfs asks
 * for at a barrier is "everything you are holding, on the device now",
 * and this device is holding nothing by construction. A fake that
 * counted flushes would be a fake with an opinion about an
 * implementation detail of the real one. */
void blk_flush(void) {
}

/* Likewise: there is nothing to read ahead OF. Accepted so that a unit
 * under test which sets it compiles and behaves identically. */
void blk_set_readahead(uint32_t lines) {
    (void)lines;
}
