#pragma once

#include <stdint.h>

#define BLOCK_DEVICE_SECTOR_SIZE 512

#define BLOCK_DEVICE_LARGE_MACHINE_FRAMES (8ull * 1024 * 1024 * 1024 / 4096)
#define BLOCK_DEVICE_MAX_CACHE_LINES      262144u

uint32_t block_device_cache_lines_for(uint64_t free_frames);

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
    uint64_t journal_commits;
    uint64_t journal_checkpoints;
    uint64_t journal_blocks_written;
    uint64_t journal_home_writes;
    uint32_t journal_replayed;
} block_device_statistics_t;

void block_device_statistics(block_device_statistics_t *out);

int block_device_flush(void);

void block_device_set_readahead(uint32_t lines);

void block_device_cache_drop(void);

int block_device_journal_attach(uint32_t fs_first_lba, uint32_t fs_sectors, uint32_t journal_first_lba,
                                uint32_t blocks);
void block_device_journal_forget(void);
int block_device_journal_reset(void);
int block_device_commit(void);
int block_device_checkpoint(void);
void block_device_commit_if_due(void);
uint32_t block_device_journal_scratch_lba(uint32_t blocks);
