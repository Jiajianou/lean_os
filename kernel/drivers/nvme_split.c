#include "nvme_split.h"

int nvme_split_next(uint64_t lba, uint32_t count, uint32_t shift,
                    uint32_t max_sectors, nvme_chunk_t *out) {
    if (count == 0) {
        return 0;
    }
    uint32_t per_block = 1u << shift;
    uint64_t block = lba >> shift;
    uint32_t offset = (uint32_t)(lba & (per_block - 1));

    if (offset != 0 || count < per_block) {
        uint32_t n = per_block - offset;
        if (n > count) {
            n = count;
        }
        out->block = block;
        out->blocks = 1;
        out->offset = offset;
        out->sectors = n;
        out->partial = 1;
        return 1;
    }

    uint32_t sectors = count & ~(per_block - 1);
    if (sectors > max_sectors) {
        sectors = max_sectors & ~(per_block - 1);
    }
    out->block = block;
    out->blocks = sectors >> shift;
    out->offset = 0;
    out->sectors = sectors;
    out->partial = 0;
    return 1;
}
