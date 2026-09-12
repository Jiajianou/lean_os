#include "nvme_split.h"

int nvme_split_next(uint64_t lba, uint32_t count, uint32_t shift,
                    uint32_t max_sectors, nvme_chunk_t *out) {
    if (count == 0) {
        return 0;
    }
    uint32_t per_block = 1u << shift; /* 512-byte sectors in one device block */
    uint64_t block = lba >> shift;
    uint32_t offset = (uint32_t)(lba & (per_block - 1));

    if (offset != 0 || count < per_block) {
        /* A head or a tail that lands inside a block. Exactly one block,
         * and only the part of it the caller asked for - never more, so
         * a read-modify-write can never touch a sector outside the
         * request. */
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

    /* The aligned middle. Whole blocks only: a tail shorter than a block
     * is left to the next call, which takes the branch above. */
    uint32_t sectors = count & ~(per_block - 1);
    if (sectors > max_sectors) {
        /* Bounded by the bounce buffer, and rounded back DOWN to a whole
         * number of device blocks - a bound that split a block would
         * hand the controller a partial command, which it refuses. */
        sectors = max_sectors & ~(per_block - 1);
    }
    out->block = block;
    out->blocks = sectors >> shift;
    out->offset = 0;
    out->sectors = sectors;
    out->partial = 0;
    return 1;
}
