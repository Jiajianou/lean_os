#pragma once

#include <stdint.h>

#define BLK_SECTOR_SIZE 512

void blk_init(void);

const char *blk_backend_name(void);

int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);

void blk_fault_inject(int64_t fail_reads_after, int64_t fail_writes_after);
uint64_t blk_error_count(void);

typedef struct {
    uint64_t reads;
    uint64_t hits;
    uint64_t device_reads;
    uint64_t device_writes;
    uint32_t resident;
    uint32_t capacity;
    uint32_t dirty;
    uint64_t writebacks;
    uint64_t readaheads;
} blk_stats_t;

void blk_stats(blk_stats_t *out);

int blk_flush(void);

void blk_set_readahead(uint32_t lines);

void blk_cache_drop(void);
