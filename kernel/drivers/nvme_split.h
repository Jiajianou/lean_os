/* kernel/drivers/nvme_split.h - M107
 *
 * How a request in 512-byte sectors becomes commands in a namespace's
 * own blocks.
 *
 * ---- why this is a unit and not four lines inside nvme.c -------------
 *
 * Because QEMU cannot test it. QEMU's emulated NVMe namespace has 512-
 * byte blocks, so every request is already aligned, `shift` is zero, and
 * the whole read-modify-write path below is dead code on the only machine
 * this project has ever run on. A 4Kn drive - one whose logical block IS
 * 4096 bytes - is an ordinary thing to find in a laptop, and the first
 * time this code would run is on hardware, on the filesystem, with no
 * serial log of what it did.
 *
 * That is the exact shape of the risk M107's "graded under QEMU first"
 * bullet is about, turned inside out: here is a path QEMU cannot grade at
 * all, so it is extracted to where something else can.
 *
 * ---- the contract, which is what the test checks ---------------------
 *
 * Repeated calls to nvme_split_next() walk a request in chunks. Each
 * chunk is either
 *
 *   - PARTIAL: one device block, of which only [offset, offset+sectors)
 *     is the caller's - which the driver serves by reading the block,
 *     patching it and writing it back; or
 *   - WHOLE: a run of complete device blocks, which goes straight
 *     through.
 *
 * The properties that must hold, and that the host test asserts over
 * every combination rather than a few: the chunks tile the request
 * exactly, in order, with no gap and no overlap; no chunk ever spans a
 * device block boundary unless it is block-aligned at both ends; no chunk
 * exceeds the bounce buffer; and a request that is already aligned
 * produces no PARTIAL chunks at all, because paying for a read-modify-
 * write on a 512-byte namespace would be a silent halving of every
 * write on the machine.
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint64_t block;    /* device block this chunk starts at */
    uint32_t blocks;   /* how many device blocks the command covers */
    uint32_t offset;   /* 512-byte sectors into the first block that are the caller's */
    uint32_t sectors;  /* how many 512-byte sectors of the caller's this chunk carries */
    int partial;       /* 1 if a read-modify-write is needed */
} nvme_chunk_t;

/* Fills *out with the next chunk of [lba, lba+count) and returns 1, or
 * returns 0 when count is 0.
 *
 * `shift` is log2(block size / 512): 0 for a 512-byte namespace, 3 for a
 * 4096-byte one. `max_sectors` bounds a WHOLE chunk to what the driver's
 * bounce buffer holds.
 *
 * The caller advances by out->sectors and calls again. It does not have
 * to know which of the two cases it is in to do that, which is the point:
 * the loop in nvme.c is the same three lines whatever the namespace is
 * formatted as. */
int nvme_split_next(uint64_t lba, uint32_t count, uint32_t shift,
                    uint32_t max_sectors, nvme_chunk_t *out);
