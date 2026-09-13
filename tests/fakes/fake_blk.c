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
static int64_t fail_reads_after = -1;
static int silent_writes;
static uint64_t errors;

void fake_blk_reset(uint32_t sectors);
void fake_blk_free(void);
void fake_blk_reset_counters(void);
uint64_t fake_blk_reads(void);
uint64_t fake_blk_writes(void);
uint8_t *fake_blk_sector(uint32_t lba);
uint32_t fake_blk_sector_count(void);
void fake_blk_fail_writes_after(int64_t n);
void fake_blk_fail_writes_silently_after(int64_t n);
void fake_blk_fail_reads_after(int64_t n);

void fake_blk_reset(uint32_t sectors) {
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
    fail_reads_after = -1;
    silent_writes = 0;
    errors = 0;
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
void fake_blk_fail_writes_after(int64_t n) {
    fail_writes_after = n;
    silent_writes = 0;
}

void fake_blk_fail_writes_silently_after(int64_t n) {
    fail_writes_after = n;
    silent_writes = 1;
}

void fake_blk_fail_reads_after(int64_t n) { fail_reads_after = n; }

uint8_t *fake_blk_sector(uint32_t lba) {
    if (!disk || lba >= disk_sectors) {
        panic("fake_blk_sector: out of range");
    }
    return disk + (size_t)lba * BLK_SECTOR_SIZE;
}

static void range_check(uint32_t lba, uint32_t count, const char *what) {
    if (!disk) {
        panic("fake_blk: access before fake_blk_reset");
    }
    if (count == 0 || lba > disk_sectors || count > disk_sectors - lba) {
        panic(what);
    }
}

int blk_read(uint32_t lba, uint32_t count, void *buf) {
    range_check(lba, count, "fake_blk: read past the end of the disk");
    if (fail_reads_after >= 0 && (int64_t)reads >= fail_reads_after) {
        memset(buf, 0, (size_t)count * BLK_SECTOR_SIZE);
        reads += count;
        errors++;
        return -1;
    }
    memcpy(buf, disk + (size_t)lba * BLK_SECTOR_SIZE, (size_t)count * BLK_SECTOR_SIZE);
    reads += count;
    return 0;
}

int blk_write(uint32_t lba, uint32_t count, const void *buf) {
    range_check(lba, count, "fake_blk: write past the end of the disk");
    if (fail_writes_after >= 0 && (int64_t)writes >= fail_writes_after) {
        writes += count;
        errors++;
        return silent_writes ? 0 : -1;
    }
    memcpy(disk + (size_t)lba * BLK_SECTOR_SIZE, buf, (size_t)count * BLK_SECTOR_SIZE);
    writes += count;
    return 0;
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

int blk_flush(void) {
    return 0;
}

void blk_fault_inject(int64_t reads_after, int64_t writes_after) {
    fail_reads_after = reads_after;
    fail_writes_after = writes_after;
    silent_writes = 0;
}

uint64_t blk_error_count(void) {
    return errors;
}

void blk_set_readahead(uint32_t lines) {
    (void)lines;
}
