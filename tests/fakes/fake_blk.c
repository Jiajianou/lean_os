#include "drivers/block_device.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *message);

static uint8_t *disk;
static uint32_t disk_sectors;
static uint64_t reads;
static uint64_t writes;
static int64_t fail_writes_after = -1;
static int64_t fail_reads_after = -1;
static int silent_writes;
static uint64_t errors;
static uint64_t read_calls;

void fake_block_device_reset(uint32_t sectors);
void fake_block_device_free(void);
void fake_block_device_reset_counters(void);
uint64_t fake_block_device_reads(void);
uint64_t fake_block_device_read_calls(void);
uint64_t fake_block_device_writes(void);
uint8_t *fake_block_device_sector(uint32_t lba);
uint32_t fake_block_device_sector_count(void);
void fake_block_device_fail_writes_after(int64_t n);
void fake_block_device_fail_writes_silently_after(int64_t n);
void fake_block_device_fail_reads_after(int64_t n);

void fake_block_device_reset(uint32_t sectors) {
    if (disk && disk_sectors == sectors) {
        memset(disk, 0, (size_t)sectors * BLOCK_DEVICE_SECTOR_SIZE);
    } else {
        free(disk);
        disk = (uint8_t *)calloc(sectors, BLOCK_DEVICE_SECTOR_SIZE);
        if (!disk) {
            panic("fake_blk: could not allocate the RAM disk");
        }
        disk_sectors = sectors;
    }
    reads = writes = read_calls = 0;
    fail_writes_after = -1;
    fail_reads_after = -1;
    silent_writes = 0;
    errors = 0;
}

void fake_block_device_free(void) {
    free(disk);
    disk = NULL;
    disk_sectors = 0;
}

void fake_block_device_reset_counters(void) { reads = writes = read_calls = 0; }
uint64_t fake_block_device_reads(void) { return reads; }
uint64_t fake_block_device_read_calls(void) { return read_calls; }
uint64_t fake_block_device_writes(void) { return writes; }
uint32_t fake_block_device_sector_count(void) { return disk_sectors; }
void fake_block_device_fail_writes_after(int64_t n) {
    fail_writes_after = n;
    silent_writes = 0;
}

void fake_block_device_fail_writes_silently_after(int64_t n) {
    fail_writes_after = n;
    silent_writes = 1;
}

void fake_block_device_fail_reads_after(int64_t n) { fail_reads_after = n; }

uint8_t *fake_block_device_sector(uint32_t lba) {
    if (!disk || lba >= disk_sectors) {
        panic("fake_blk_sector: out of range");
    }
    return disk + (size_t)lba * BLOCK_DEVICE_SECTOR_SIZE;
}

static void range_check(uint32_t lba, uint32_t count, const char *what) {
    if (!disk) {
        panic("fake_blk: access before fake_blk_reset");
    }
    if (count == 0 || lba > disk_sectors || count > disk_sectors - lba) {
        panic(what);
    }
}

int block_device_read(uint32_t lba, uint32_t count, void *buffer) {
    range_check(lba, count, "fake_blk: read past the end of the disk");
    /* M176: sectors say how much was moved, calls say how many times the
       device was asked. Coalescing neighbouring blocks changes only the
       second, so only the second can grade it. */
    read_calls++;
    if (fail_reads_after >= 0 && (int64_t)reads >= fail_reads_after) {
        memset(buffer, 0, (size_t)count * BLOCK_DEVICE_SECTOR_SIZE);
        reads += count;
        errors++;
        return -1;
    }
    memcpy(buffer, disk + (size_t)lba * BLOCK_DEVICE_SECTOR_SIZE, (size_t)count * BLOCK_DEVICE_SECTOR_SIZE);
    reads += count;
    return 0;
}

int block_device_write(uint32_t lba, uint32_t count, const void *buffer) {
    range_check(lba, count, "fake_blk: write past the end of the disk");
    if (fail_writes_after >= 0 && (int64_t)writes >= fail_writes_after) {
        writes += count;
        errors++;
        return silent_writes ? 0 : -1;
    }
    memcpy(disk + (size_t)lba * BLOCK_DEVICE_SECTOR_SIZE, buffer, (size_t)count * BLOCK_DEVICE_SECTOR_SIZE);
    writes += count;
    return 0;
}

void block_device_init(void) {}
const char *block_device_backend_name(void) { return "fake-ram"; }
void block_device_cache_drop(void) {}

void block_device_statistics(block_device_statistics_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->reads = reads;
    out->device_reads = reads;
    out->device_writes = writes;
}

int block_device_flush(void) {
    return 0;
}

void block_device_fault_inject(int64_t reads_after, int64_t writes_after) {
    fail_reads_after = reads_after;
    fail_writes_after = writes_after;
    silent_writes = 0;
}

uint64_t block_device_error_count(void) {
    return errors;
}

void block_device_set_readahead(uint32_t lines) {
    (void)lines;
}
