#include "blk.h"

#include "drivers/ata.h"
#include "drivers/klog.h"
#include "drivers/virtio_blk.h"
#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/pmm.h"

/* One cache line is eight sectors - 4 KiB, a page, and the unit leanfs's
 * own block size already is. Caching at sector granularity would triple
 * the tag overhead for the same bytes and would not match a single access
 * pattern in the filesystem above it. */
#define BLK_PER_LINE 8
#define LINE_BYTES   (BLK_PER_LINE * BLK_SECTOR_SIZE)

/* 2048 lines is 8 MiB of cache.
 *
 * The number is chosen against what it has to hold rather than against
 * what the machine has. leanfs's inode table is 2048 sectors (M81) and
 * its block bitmap is 16 more; a directory scan walks those repeatedly,
 * and everything else is data blocks that are read once. 8 MiB holds the
 * whole metadata region several times over with room for the working set
 * of a tree walk, on a machine M90 established has gigabytes. It is a
 * fixed allocation rather than a fraction of memory because a cache whose
 * size depends on the machine is a cache whose measurements do not
 * compare between two boots. */
#define CACHE_LINES 2048

typedef struct {
    uint32_t line_no;  /* lba / BLK_PER_LINE */
    uint8_t  valid;
} cache_tag_t;

static cache_tag_t tags[CACHE_LINES];
/* One frame per line, and deliberately NOT one contiguous run.
 *
 * The first version of this allocated 2048 frames and required them to
 * come back consecutive, on the theory that a fresh allocator hands out
 * consecutive frames. That theory is false here and the reason is worth
 * recording: blk_init runs late in boot, after every self-test that
 * allocates and frees - M90's own probe frees a frame at 4 GiB and pulls
 * the allocator's search hint back with it - so the free list is holed by
 * the time this asks. A line is exactly one page, so there is no reason
 * to need contiguity at all, and an array of pointers costs 16 KiB.
 *
 * pmm_alloc_frame rather than pmm_alloc_contiguous for the same reason:
 * the cache is never handed to a device, so taking megabytes out of the
 * one region a 32-bit DMA engine can reach (M90's PMM_DMA_LIMIT) would be
 * taking it from the one place a driver cannot do without. */
static uint8_t *line_ptr[CACHE_LINES];
static uint32_t cache_lines; /* how many of them actually exist - see blk_init */

static int have_virtio;
static blk_stats_t stats;

/* One lock over the whole cache. leanfs already serialises itself above
 * this, and every access here is short; a finer-grained scheme would be
 * complexity with no contention to relieve. Deliberately not a leaf lock
 * in the pmm sense - blk_init allocates, but nothing after it does. */
static spinlock_t blk_lock;

static void device_read(uint32_t lba, uint32_t count, void *buf) {
    stats.device_reads += count;
    if (have_virtio) {
        virtio_blk_read(lba, count, buf);
        return;
    }
    /* ata_read_sectors takes a uint8_t count, so a run longer than 255
     * has to be split - the same loop leanfs used to carry itself. */
    uint8_t *dst = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        ata_read_sectors(lba, (uint8_t)n, dst);
        dst += n * BLK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
}

static void device_write(uint32_t lba, uint32_t count, const void *buf) {
    stats.device_writes += count;
    if (have_virtio) {
        virtio_blk_write(lba, count, buf);
        return;
    }
    const uint8_t *src = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        ata_write_sectors(lba, (uint8_t)n, src);
        src += n * BLK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
}

void blk_init(void) {
    have_virtio = virtio_blk_init();

    /* Sized against what the machine has as well as against what the
     * filesystem needs. CACHE_LINES is the ceiling; a sixteenth of free
     * memory is the floor under it, so the 128 MiB machine M90's second
     * run proves this kernel still boots on does not hand an eighth of
     * itself to a cache. The number that came out is logged, because a
     * measurement taken against a cache of unknown size is not a
     * measurement. */
    cache_lines = CACHE_LINES;
    uint64_t affordable = pmm_free_frame_count() / 16;
    if (affordable < cache_lines) {
        cache_lines = (uint32_t)affordable;
    }
    if (cache_lines < 64) {
        cache_lines = 64; /* below this it is not a cache, it is a buffer */
    }

    for (uint32_t i = 0; i < cache_lines; i++) {
        line_ptr[i] = (uint8_t *)(uintptr_t)pmm_alloc_frame();
        tags[i].valid = 0;
    }
    stats.capacity = cache_lines;

    klog_puts("[blk] ");
    klog_puts(blk_backend_name());
    klog_puts(", ");
    klog_put_dec((cache_lines * LINE_BYTES) / 1024);
    klog_puts(" KiB write-through cache in ");
    klog_put_dec(cache_lines);
    klog_puts(" lines of ");
    klog_put_dec(LINE_BYTES);
    klog_puts(" bytes\n");
}

const char *blk_backend_name(void) {
    return have_virtio ? "virtio-blk" : "ata-pio";
}

/* Direct-mapped: a line's home is its own number modulo the cache size.
 *
 * Deliberately not set-associative, and the argument is the same one M87
 * made for a three-entry mount table searched by prefix: an index that is
 * one modulo is faster than anything cleverer and easier to be sure of.
 * The pathological case - two hot lines whose numbers differ by exactly
 * cache_lines - costs a miss each time, and the filesystem's hot region
 * (the inode table) is contiguous, which is the shape a direct-mapped
 * cache handles best rather than worst. If a measurement ever shows
 * thrashing, associativity is the answer and this is where it goes. */
static uint32_t slot_of(uint32_t line_no) {
    return line_no % cache_lines;
}

static int all_lines_resident(uint32_t first_line, uint32_t last_line) {
    for (uint32_t ln = first_line; ln <= last_line; ln++) {
        uint32_t s = slot_of(ln);
        if (!(tags[s].valid && tags[s].line_no == ln)) {
            return 0;
        }
    }
    return 1;
}

/* ---- One request, not one per line ------------------------------------
 *
 * The obvious cache is per-line: for each line the caller touches, fetch
 * it if absent, then copy out. That is correct and it throws away most of
 * what the new driver bought. leanfs reads its 2048-sector inode table in
 * one call; a per-line fetch turns that into 256 separate device requests
 * of eight sectors each, and on a device where the cost is per *request*
 * rather than per byte - which is exactly what changes when PIO becomes
 * DMA - that is 256 round trips to avoid one.
 *
 * So the decision is made once for the whole range. If every line it
 * touches is already resident, it is served entirely from memory. If any
 * is missing, the whole range is read in a single request straight into
 * the caller's buffer, and the cache is then populated from what came
 * back - for the lines the range covers completely, since a partly
 * covered line at either end cannot be filled from this buffer alone.
 *
 * Populating from the caller's buffer rather than re-reading is safe for
 * one reason and it is the same reason eviction is free: the cache is
 * write-through, so what is on the disk and what is in a resident line
 * are the same bytes by construction. There is no version of this where
 * the copy could be stale. */
void blk_read(uint32_t lba, uint32_t count, void *buf) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    stats.reads++;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t first_line = lba / BLK_PER_LINE;
    uint32_t last_line = (lba + count - 1) / BLK_PER_LINE;

    if (all_lines_resident(first_line, last_line)) {
        stats.hits++;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = slot_of((lba + i) / BLK_PER_LINE);
            uint32_t within = (lba + i) % BLK_PER_LINE;
            k_memcpy(dst + (uint64_t)i * BLK_SECTOR_SIZE,
                     line_ptr[s] + (uint64_t)within * BLK_SECTOR_SIZE,
                     BLK_SECTOR_SIZE);
        }
        spin_unlock_irqrestore(&blk_lock, irq);
        return;
    }

    device_read(lba, count, dst);

    for (uint32_t ln = first_line; ln <= last_line; ln++) {
        uint32_t line_lba = ln * BLK_PER_LINE;
        if (line_lba < lba || line_lba + BLK_PER_LINE > lba + count) {
            continue; /* only partly covered by this request */
        }
        uint32_t s = slot_of(ln);
        if (!tags[s].valid) {
            stats.resident++;
        }
        k_memcpy(line_ptr[s], dst + (uint64_t)(line_lba - lba) * BLK_SECTOR_SIZE, LINE_BYTES);
        tags[s].line_no = ln;
        tags[s].valid = 1;
    }
    spin_unlock_irqrestore(&blk_lock, irq);
}

void blk_write(uint32_t lba, uint32_t count, const void *buf) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    /* The device first, in the caller's order, because that ordering is
     * what M71's guarantee is made of. The cache is updated afterwards
     * and only for lines that are already resident - pulling a line in
     * on a write would turn every sequential write into a read-modify-
     * write of a 4 KiB line, which is exactly the traffic an unpack must
     * not generate. */
    device_write(lba, count, buf);
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t line_no = (lba + i) / BLK_PER_LINE;
        uint32_t within = (lba + i) % BLK_PER_LINE;
        uint32_t s = slot_of(line_no);
        if (tags[s].valid && tags[s].line_no == line_no) {
            k_memcpy(line_ptr[s] + (uint64_t)within * BLK_SECTOR_SIZE,
                     src + (uint64_t)i * BLK_SECTOR_SIZE, BLK_SECTOR_SIZE);
        }
    }
    spin_unlock_irqrestore(&blk_lock, irq);
}

void blk_stats(blk_stats_t *out) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    *out = stats;
    spin_unlock_irqrestore(&blk_lock, irq);
}

void blk_cache_drop(void) {
    uint64_t irq = spin_lock_irqsave(&blk_lock);
    for (uint32_t i = 0; i < cache_lines; i++) {
        tags[i].valid = 0;
    }
    stats.resident = 0;
    spin_unlock_irqrestore(&blk_lock, irq);
}
