#pragma once

#include <stdint.h>

#define BLOCK_DEVICE_SECTOR_SIZE 512

void block_device_init(void);

const char *block_device_backend_name(void);

int block_device_read(uint32_t lba, uint32_t count, void *buffer);
int block_device_write(uint32_t lba, uint32_t count, const void *buffer);

void block_device_fault_inject(int64_t fail_reads_after, int64_t fail_writes_after);
uint64_t block_device_error_count(void);

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
} block_device_statistics_t;

void block_device_statistics(block_device_statistics_t *out);

int block_device_flush(void);

void block_device_set_readahead(uint32_t lines);

void block_device_cache_drop(void);
