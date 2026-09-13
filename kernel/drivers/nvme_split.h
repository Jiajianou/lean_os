#pragma once

#include <stdint.h>

typedef struct {
    uint64_t block;
    uint32_t blocks;
    uint32_t offset;
    uint32_t sectors;
    int partial;
} nvme_chunk_t;

int nvme_split_next(uint64_t lba, uint32_t count, uint32_t shift,
                    uint32_t max_sectors, nvme_chunk_t *out);
