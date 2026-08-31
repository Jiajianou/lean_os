#include "leanfs.h"

#include "drivers/ata.h"
#include "drivers/klog.h"
#include "drivers/rtc.h"
#include "lib/libk.h"

#define LEANFS_MAGIC     0x3453464Cu /* "LFS4". M81: bumped from M59's 0x3353464C (itself M53's 0x3253464C, itself M12's 0x3153464C). Every region moved: the inode table grew from 32 sectors to 2048, which pushes the bitmap and the whole data region down the disk, and directory records stopped being fixed-size. An old disk read with this layout would resolve garbage block numbers, so it is reformatted rather than misread - see leanfs_init, where M81 also adds the version field that makes a *future* bump able to do better than that. */
#define LEANFS_VERSION   4u          /* M81: see sb.version. Bumped only when the on-disk meaning changes; the magic is bumped only when the geometry does. */
#define LEANFS_START_LBA 8192u /* M83: 2048 -> 8192. 4 MiB in, because the boot image reached 2049 sectors and the Makefile's size guard refused to build - see the FS_START_LBA note there, which also records that this filesystem had been overlapping the EFI System Partition since M81. Must match Makefile's FS_START_LBA. */

#define LEANFS_TYPE_FREE 0
#define LEANFS_TYPE_FILE 1
#define LEANFS_TYPE_DIR  2

#define ROOT_INODE 0

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t bitmap_lba;
    uint32_t bitmap_sectors;
    uint32_t data_lba;
    uint32_t data_blocks;
    /* M71: was `reserved`. Set to LEANFS_STATE_DIRTY on mount and back to
     * LEANFS_STATE_CLEAN by leanfs_sync (which the orderly shutdown path
     * calls). A filesystem found DIRTY on mount was not unmounted - the
     * machine lost power, panicked, or was killed - and gets checked
     * before it is trusted. Reusing the reserved word rather than bumping
     * the magic on purpose: an old disk reads 0 here, which is exactly
     * LEANFS_STATE_CLEAN, so every existing filesystem is treated as
     * cleanly unmounted the first time this runs. That is the right
     * default - it was, by construction, written by a kernel that had no
     * way to leave it otherwise. */
    uint32_t state;
    /* M81: the format version, distinct from the magic on purpose.
     *
     * The magic answers "is this a leanfs, laid out the way this build
     * expects" - it changes when a region moves, and a mismatch can only
     * ever be a reformat, because there is nowhere to stand to read the
     * old disk. The version answers "which revision of the format is
     * this" for changes that leave the geometry alone - a new inode
     * field landing in the padding, a new record flag - and those a
     * future build genuinely can migrate in place.
     *
     * M81 itself is a geometry change and therefore reformats; the field
     * exists so that the next one does not have to. An old disk reads 0
     * here, which is not LEANFS_VERSION, and is caught by the magic
     * first anyway. */
    uint32_t version;
} leanfs_superblock_t;

#define LEANFS_STATE_CLEAN 0u
#define LEANFS_STATE_DIRTY 0x4449525Au /* "DIRZ" - a value no zeroed disk produces by accident */

/* M53: no name field any more. A name lives in exactly one place - the
 * records of the directory that holds it - which is what makes two
 * disagreeing answers to "what is this file called" impossible rather
 * than merely unlikely. `type` doubles as the allocated/free flag the
 * old `used` was. */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    /* M59: seconds since 1970, from kernel/drivers/rtc.h. 0 means "no
     * clock on this machine, or written before there was one" - the same
     * value for both, which is honest: neither can say when this file was
     * written, and inventing a different sentinel for each would suggest
     * one of them could. */
    uint32_t mtime;
    uint32_t direct[LEANFS_DIRECT_BLOCKS];
    uint32_t indirect;  /* block number of a table of 128 block numbers */
    /* M59: a table of 128 *indirect* tables. 16 direct plus 128 indirect
     * was a 72 KiB ceiling - smaller than compositor.c's own source - and
     * a seek API on a file that cannot be large is a much smaller feature
     * than it looks. Deliberately stops here: a third level would be
     * another order of magnitude past anything this OS can hold. */
    uint32_t dindirect;
    /* M81: pad to a round 128 bytes. Two reasons, and the second is the
     * one that matters.
     *
     * Four inodes per sector exactly means mark_inode never has to widen
     * a range across a sector boundary, and an inode table of N inodes is
     * exactly N/4 sectors with no rounding - which is what makes the
     * table writable straight out of `inodes[]` with no staging buffer
     * (see save_meta, which used to need a 16 KiB one).
     *
     * And it leaves 44 bytes that a field can land in without moving
     * anything after it - which is the difference between the next
     * format change being a version bump and being a reformat. M87 is
     * scheduled to add a link count and a symlink target; this is the
     * room for them, reserved because that is scheduled work and not
     * because a field might one day be nice. */
    uint8_t  reserved[44];
} leanfs_inode_t;

_Static_assert(sizeof(leanfs_inode_t) == 128, "an inode must be 128 bytes so four fit a sector exactly");
_Static_assert(LEANFS_BLOCK_SIZE % sizeof(leanfs_inode_t) == 0, "an inode must not straddle a sector");

#define INODE_TABLE_SECTORS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_SECTORS       (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE)

/* M81: the record header is variable-length now, so the old "16 fit a
 * block exactly" assertion is gone - the no-straddling guarantee it
 * protected is enforced instead by dir_block_place and dir_block_walk,
 * which never let a rec_len run past the end of its block. What is still
 * a compile-time claim is the header's own size, because
 * LEANFS_DIRENT_NEED in leanfs.h computes with the number rather than
 * the type. */
_Static_assert(sizeof(leanfs_dirent_t) == LEANFS_DIRENT_HDR, "leanfs_dirent_t header must stay 8 bytes");
_Static_assert(LEANFS_DIRENT_NEED(LEANFS_MAX_NAME) <= LEANFS_BLOCK_SIZE, "the longest name must still fit in one block");

static leanfs_superblock_t sb;

/* M81: the inode table is written to disk straight out of this array.
 *
 * It used to be staged through a separate sector-sized `inode_table_buf`
 * because 192 * 84 bytes is not a whole number of sectors and the tail of
 * the last one had to come from somewhere. An inode is 128 bytes now
 * (see leanfs_inode_t), so the array *is* a whole number of sectors by
 * construction, and the staging buffer - which had grown to a second full
 * copy of a table that is now 1 MiB - is gone rather than doubled. */
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[BITMAP_SECTORS * LEANFS_BLOCK_SIZE];

_Static_assert(sizeof(inodes) % LEANFS_BLOCK_SIZE == 0,
               "the inode table must be a whole number of sectors so save_meta can write it in place");

/* M81: one directory block, and that is the whole scratch space
 * directories need now.
 *
 * Before this, dir_load unpacked an entire directory into a
 * dirent_scratch sized at LEANFS_MAX_DIRENTS records and dir_store wrote
 * all of it back - which was 6 KiB and one sector's worth of I/O at 192
 * inodes, and would have been 2 MiB and four thousand sector writes per
 * created file at 8192. Records are addressed a block at a time instead,
 * so creating a file writes the one block its name landed in.
 *
 * Shared rather than on the stack for M53's original reason (a kernel
 * stack has better things to do) and safe for M67's: every entry point
 * into this file is called under vfs.c's fs_lock with interrupts off. */
static uint8_t dir_block[LEANFS_BLOCK_SIZE];

/* M81: the last block of the last directory an insert landed in. See
 * dir_add for what it buys and why it is safe to be wrong. Reset by
 * dir_remove when it frees something below it, so a hole is never hidden
 * behind the hint. */
static int dir_hint_inode = -1;
static uint32_t dir_hint_block = 0;

static void save_superblock(void) {
    uint8_t buf[LEANFS_BLOCK_SIZE];
    k_memset(buf, 0, sizeof(buf));
    k_memcpy(buf, &sb, sizeof(sb));
    ata_write_sectors(LEANFS_START_LBA, 1, buf);
}

/* ---- M59: dirty-sector metadata tracking ------------------------------
 *
 * Every metadata flush used to write the superblock, the entire inode
 * table and the entire bitmap - 32 PIO sector writes whether one byte had
 * changed or seventy kilobytes had. M56 already measured PIO writes as
 * the most expensive thing this OS does, in a self-test that had to be
 * redesigned around it, and `Ctrl+S` in the editor is where a person
 * feels it.
 *
 * The fix is not a write cache (leanfs stays write-through - vfs_sync's
 * own comment depends on that being true), it is knowing *which sectors
 * changed*. A one-byte edit to an existing file now touches one inode
 * sector and nothing else: one write instead of thirty-two.
 *
 * Correctness rests on one rule, and it is the only rule: nothing may
 * mutate `inodes[]` or `bitmap[]` without marking the sector it landed
 * in. mark_inode/mark_block are the only way to do that, and every
 * mutation below goes through them.
 */
static uint8_t inode_sector_dirty[INODE_TABLE_SECTORS];
static uint8_t bitmap_sector_dirty[BITMAP_SECTORS];

/* M59: how many metadata sectors this filesystem has actually written
 * since boot. Exported (leanfs_meta_writes) so a test can assert the
 * *cost* of a one-byte save rather than its latency, which is a property
 * of the host rather than of this code. */
static uint32_t meta_writes;

static void mark_inode(int idx) {
    size_t byte = (size_t)idx * sizeof(leanfs_inode_t);
    size_t first = byte / LEANFS_BLOCK_SIZE;
    size_t last = (byte + sizeof(leanfs_inode_t) - 1) / LEANFS_BLOCK_SIZE;
    for (size_t sec = first; sec <= last && sec < INODE_TABLE_SECTORS; sec++) {
        inode_sector_dirty[sec] = 1;
    }
}

static void mark_all_inodes(void) {
    for (size_t i = 0; i < INODE_TABLE_SECTORS; i++) {
        inode_sector_dirty[i] = 1;
    }
}

static void mark_block_bit(uint32_t bit) {
    size_t sec = (bit / 8u) / LEANFS_BLOCK_SIZE;
    if (sec < BITMAP_SECTORS) {
        bitmap_sector_dirty[sec] = 1;
    }
}

static void mark_all_blocks(void) {
    for (size_t i = 0; i < BITMAP_SECTORS; i++) {
        bitmap_sector_dirty[i] = 1;
    }
}

/* M81: ata_write_sectors takes a uint8_t count, so a run longer than 255
 * sectors cannot be expressed - and 255 truncates to 0, which the drive
 * reads as "256" while the driver's own loop writes none of them. That
 * was unreachable while the inode table was 32 sectors and is very
 * reachable now that it is 2048, so every bulk write goes through here.
 *
 * Deliberately fixed at the call site rather than by widening the ATA
 * driver's signature: 255 is what 28-bit LBA PIO actually encodes in one
 * command, so the chunking is the hardware's, not this filesystem's. */
#define ATA_MAX_RUN 255u

static void write_run(uint32_t lba, size_t sectors, const uint8_t *src) {
    while (sectors > 0) {
        size_t n = sectors > ATA_MAX_RUN ? ATA_MAX_RUN : sectors;
        ata_write_sectors(lba, (uint8_t)n, src);
        lba += (uint32_t)n;
        src += n * LEANFS_BLOCK_SIZE;
        sectors -= n;
    }
}

static void read_run(uint32_t lba, size_t sectors, uint8_t *dst) {
    while (sectors > 0) {
        size_t n = sectors > ATA_MAX_RUN ? ATA_MAX_RUN : sectors;
        ata_read_sectors(lba, (uint8_t)n, dst);
        lba += (uint32_t)n;
        dst += n * LEANFS_BLOCK_SIZE;
        sectors -= n;
    }
}

/* Writes only what changed. Runs of adjacent dirty sectors go out as one
 * write_run call, because a run is exactly as cheap as a single sector to
 * set up and this is the path a whole-file write takes. */
static void save_meta(void) {
    const uint8_t *inode_bytes = (const uint8_t *)inodes;

    for (size_t i = 0; i < INODE_TABLE_SECTORS; ) {
        if (!inode_sector_dirty[i]) {
            i++;
            continue;
        }
        size_t run = 0;
        while (i + run < INODE_TABLE_SECTORS && inode_sector_dirty[i + run]) {
            inode_sector_dirty[i + run] = 0;
            run++;
        }
        write_run(sb.inode_table_lba + (uint32_t)i, run,
                  inode_bytes + i * LEANFS_BLOCK_SIZE);
        meta_writes += (uint32_t)run;
        i += run;
    }

    for (size_t i = 0; i < BITMAP_SECTORS; ) {
        if (!bitmap_sector_dirty[i]) {
            i++;
            continue;
        }
        size_t run = 0;
        while (i + run < BITMAP_SECTORS && bitmap_sector_dirty[i + run]) {
            bitmap_sector_dirty[i + run] = 0;
            run++;
        }
        write_run(sb.bitmap_lba + (uint32_t)i, run,
                  bitmap + i * LEANFS_BLOCK_SIZE);
        meta_writes += (uint32_t)run;
        i += run;
    }
}

uint32_t leanfs_meta_writes(void) {
    return meta_writes;
}

static void format(void) {
    klog_puts("[fs] no valid leanfs superblock found - formatting fresh\n");

    sb.magic = LEANFS_MAGIC;
    sb.inode_table_lba = LEANFS_START_LBA + 1;
    sb.inode_table_sectors = INODE_TABLE_SECTORS;
    sb.bitmap_lba = sb.inode_table_lba + INODE_TABLE_SECTORS;
    sb.bitmap_sectors = BITMAP_SECTORS;
    sb.data_lba = sb.bitmap_lba + BITMAP_SECTORS;
    sb.data_blocks = LEANFS_DATA_BLOCKS;
    sb.state = LEANFS_STATE_CLEAN;
    sb.version = LEANFS_VERSION;

    k_memset(inodes, 0, sizeof(inodes));
    k_memset(bitmap, 0, sizeof(bitmap));
    /* M59: block 0 is reserved forever so that a zero block pointer can
     * mean "nothing here" - see block_present. Costs 512 bytes of a
     * 32 MiB data region. Set directly rather than through bitmap_set,
     * which is defined further down with the rest of the allocator. */
    bitmap[0] |= 1u;
    mark_block_bit(0);

    /* M53: the root directory is inode 0 and exists from the moment the
     * filesystem does. Empty (size 0, no blocks) - a directory with no
     * entries needs no storage, and dir_add below allocates on demand. */
    inodes[ROOT_INODE].type = LEANFS_TYPE_DIR;
    inodes[ROOT_INODE].size = 0;

    save_superblock();
    mark_all_inodes();
    mark_all_blocks();
    save_meta();
}

void leanfs_init(void) {
    uint8_t buf[LEANFS_BLOCK_SIZE];
    ata_read_sectors(LEANFS_START_LBA, 1, buf);
    k_memcpy(&sb, buf, sizeof(sb));

    /* M29: every field checked here sizes a fixed-size buffer somewhere
     * downstream (data_blocks -> the static `bitmap` array every
     * alloc_block/bitmap_test loop trusts as its own bound;
     * inode_table_sectors/bitmap_sectors -> exactly how many sectors
     * leanfs_init itself is about to read) - so any mismatch means a
     * superblock this build cannot safely interpret. Reformatting (the
     * one recovery path this driver already has and exercises on a blank
     * disk) is what keeps a corrupted field from turning into an
     * out-of-bounds write instead of just losing whatever was here.
     *
     * M53: the magic bump makes that path do double duty as the format
     * migration. There is no in-place upgrade from the flat layout - it
     * had no root directory and nothing to hang one off - and the kernel
     * re-seeds every program it ships on a fresh filesystem anyway. */
    if (sb.magic != LEANFS_MAGIC ||
        sb.data_blocks != LEANFS_DATA_BLOCKS ||
        sb.inode_table_sectors != INODE_TABLE_SECTORS ||
        sb.bitmap_sectors != BITMAP_SECTORS) {
        format();
    } else if (sb.version != LEANFS_VERSION) {
        /* M81: the geometry matches but the format revision does not.
         *
         * This is the branch the version field exists for, and today it
         * is unreachable - LEANFS_VERSION has only ever been 4 and the
         * magic that guards it was bumped in the same commit. It is
         * written now, and written as a reformat, so that the next
         * milestone to change the meaning of a field has a place to put
         * a migration and a visible reminder that leaving it a reformat
         * is a decision rather than an oversight.
         *
         * The honest reason M81 itself could not migrate: the inode
         * table grew by 2016 sectors, which moves the bitmap and every
         * data block on the disk. An in-place upgrade would have to
         * relocate the entire data region, and there is nowhere to
         * relocate it *to* on a disk that is already sized to hold it.
         * A format change that only reinterprets bytes can migrate; one
         * that moves them cannot. */
        klog_puts("[fs] leanfs on-disk version is not this build's - reformatting\n");
        format();
    } else {
        read_run(sb.inode_table_lba, sb.inode_table_sectors, (uint8_t *)inodes);
        read_run(sb.bitmap_lba, sb.bitmap_sectors, bitmap);

        /* A filesystem whose root is not a directory is one nothing can
         * be resolved against - reformat rather than fail every path. */
        if (inodes[ROOT_INODE].type != LEANFS_TYPE_DIR) {
            klog_puts("[fs] leanfs root inode is not a directory - reformatting\n");
            format();
        } else if (sb.state == LEANFS_STATE_DIRTY) {
            /* M71: this filesystem was never unmounted. Check it before
             * trusting it - see leanfs_check. */
            klog_puts("[fs] leanfs was not cleanly unmounted - checking\n");
            leanfs_check();
        }
    }

    /* M71: mark it in use. From here until leanfs_sync says otherwise,
     * a disk read of this superblock says "the machine was still running
     * when this was written", which is the only way a filesystem with no
     * journal can tell a clean shutdown from a power cut. */
    sb.state = LEANFS_STATE_DIRTY;
    save_superblock();

    klog_puts("[fs] leanfs ready: data_lba=0x");
    klog_put_hex32(sb.data_lba);
    klog_puts(" data_blocks=0x");
    klog_put_hex32(sb.data_blocks);
    klog_puts(" inodes=0x");
    klog_put_hex32(LEANFS_MAX_INODES);
    klog_putc('\n');
}

/* ---- blocks ---------------------------------------------------------- */

static int bitmap_test(uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
    mark_block_bit(bit); /* M59: the only two places the bitmap changes - see save_meta */
}

static void bitmap_clear(uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    mark_block_bit(bit);
}

/* M29: every block number this driver ever acts on either came straight
 * out of alloc_block (always < sb.data_blocks by construction) or off
 * disk (an inode's direct[]/indirect fields, or an indirect table's
 * entries) - the latter is trusted nowhere else, so a single bit flip
 * there would otherwise walk bitmap_clear/ata_read_sectors off the end of
 * the fixed-size `bitmap` array or into an arbitrary disk LBA. */
static int block_valid(uint32_t block) {
    return block < sb.data_blocks;
}

/* M59: is this inode pointer naming a real block?
 *
 * Block 0 is deliberately never allocated (see format), which is what
 * makes zero usable as "no block here" - and a zeroed inode field is
 * exactly what a freshly created file has. Before M59 nothing needed the
 * distinction: every block of a file was allocated in one pass and the
 * pointers were only ever read back, never tested for absence. The
 * on-demand allocator does test, and the first thing it did without this
 * was hand every new file block 0 over and over, which showed up as
 * "vfs_write: failed to seed a program onto disk" on the very next boot.
 *
 * One reserved block out of 65536 is a cheaper sentinel than widening
 * every pointer or carrying a parallel bitmap of which ones are set. */
static int block_present(uint32_t block) {
    return block != 0 && block_valid(block);
}

static int alloc_block(void) {
    for (uint32_t i = 0; i < sb.data_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return (int)i;
        }
    }
    return -1;
}

static int inode_valid(int idx) {
    return idx >= 0 && idx < LEANFS_MAX_INODES && inodes[idx].type != LEANFS_TYPE_FREE;
}

static int find_free_inode(void) {
    for (int i = 0; i < LEANFS_MAX_INODES; i++) {
        if (inodes[i].type == LEANFS_TYPE_FREE) {
            return i;
        }
    }
    return -1;
}

/* ---- M59: logical block -> physical block --------------------------
 *
 * Every read, write, grow and free below goes through this one function,
 * which is the whole reason double indirection was worth adding rather
 * than dreaded: before it, the direct/indirect split was open-coded four
 * separate times (read, write, free, and the write's own rollback path)
 * and adding a third level would have meant getting the same thing right
 * four more times.
 *
 * `logical` is a block index into the file. With allocate == 0 this only
 * looks things up and returns -1 for a hole or a corrupt pointer; with
 * allocate == 1 it creates whatever is missing on the way down, including
 * the pointer tables themselves. Returns the physical block number, or -1.
 */
static int64_t map_block(int idx, uint32_t logical, int allocate) {
    leanfs_inode_t *inode = &inodes[idx];
    uint32_t table[LEANFS_INDIRECT_POINTERS];

    if (logical >= (uint32_t)LEANFS_MAX_FILE_BLOCKS) {
        return -1;
    }

    if (logical < LEANFS_DIRECT_BLOCKS) {
        if (block_present(inode->direct[logical])) {
            return inode->direct[logical];
        }
        if (!allocate) {
            return -1;
        }
        int blk = alloc_block();
        if (blk < 0) {
            return -1;
        }
        inode->direct[logical] = (uint32_t)blk;
        mark_inode(idx);
        return blk;
    }

    /* Which of the two indirect levels this block lives in, and where
     * inside it. Written as one derivation rather than two so the split
     * point cannot drift between the read path and the write path. */
    uint32_t rel = logical - LEANFS_DIRECT_BLOCKS;
    uint32_t table_block;
    uint32_t slot;
    /* The top table of whichever level this block lives in, read out of
     * the inode by value rather than by pointer - `leanfs_inode_t` is
     * packed, so a `uint32_t *` into it is an unaligned pointer the
     * compiler is right to refuse. */
    int use_dindirect = rel >= (uint32_t)LEANFS_INDIRECT_POINTERS;
    uint32_t root = use_dindirect ? inode->dindirect : inode->indirect;
    uint32_t outer = 0;

    if (!use_dindirect) {
        slot = rel;
    } else {
        uint32_t drel = rel - (uint32_t)LEANFS_INDIRECT_POINTERS;
        outer = drel / (uint32_t)LEANFS_INDIRECT_POINTERS;
        slot = drel % (uint32_t)LEANFS_INDIRECT_POINTERS;
    }

    if (!block_present(root)) {
        if (!allocate) {
            return -1;
        }
        int t = alloc_block();
        if (t < 0) {
            return -1;
        }
        k_memset(table, 0, sizeof(table));
        ata_write_sectors(sb.data_lba + (uint32_t)t, 1, (const uint8_t *)table);
        root = (uint32_t)t;
        if (use_dindirect) {
            inode->dindirect = root;
        } else {
            inode->indirect = root;
        }
        mark_inode(idx);
    }

    if (!use_dindirect) {
        table_block = root;
    } else {
        /* The outer table names inner tables; the inner one names blocks. */
        ata_read_sectors(sb.data_lba + root, 1, (uint8_t *)table);
        if (!block_present(table[outer])) {
            if (!allocate) {
                return -1;
            }
            int t = alloc_block();
            if (t < 0) {
                return -1;
            }
            table[outer] = (uint32_t)t;
            ata_write_sectors(sb.data_lba + root, 1, (const uint8_t *)table);
            uint32_t empty[LEANFS_INDIRECT_POINTERS];
            k_memset(empty, 0, sizeof(empty));
            ata_write_sectors(sb.data_lba + (uint32_t)t, 1, (const uint8_t *)empty);
        }
        table_block = table[outer];
    }

    ata_read_sectors(sb.data_lba + table_block, 1, (uint8_t *)table);
    if (block_present(table[slot])) {
        return table[slot];
    }
    if (!allocate) {
        return -1;
    }
    int blk = alloc_block();
    if (blk < 0) {
        return -1;
    }
    table[slot] = (uint32_t)blk;
    ata_write_sectors(sb.data_lba + table_block, 1, (const uint8_t *)table);
    return blk;
}

/* Frees every block currently backing inode - its data blocks and every
 * pointer table that held them. Walked through map_block so it cannot
 * disagree with the allocator about where a block lives; the tables
 * themselves are freed afterwards, because freeing one first would leave
 * nothing to read the blocks it names out of. */
/* ---- M71: the consistency check ---------------------------------------
 *
 * Runs on mount when the superblock says this filesystem was never
 * unmounted - the machine lost power, panicked, or was killed. It is not
 * a journal replay, because there is no journal: leanfs has one writer
 * and write-through metadata, so what a crash can leave behind is not a
 * torn transaction but a *leak* - blocks the bitmap says are in use that
 * no live inode points at.
 *
 * That is the failure this filesystem actually has. Every write allocates
 * blocks, then updates the inode, then updates the directory; a crash
 * between the first and second steps leaves allocated blocks nothing
 * refers to, and nothing had ever reclaimed them. They are invisible -
 * the filesystem works perfectly - right up until the disk is full of
 * blocks belonging to files that never existed.
 *
 * So the check rebuilds the bitmap from the inodes rather than trusting
 * it. Anything the rebuild says is free and the old bitmap said was used
 * is an orphan, and gets counted and reclaimed. Anything the rebuild says
 * is USED and the bitmap said was free is far more serious - two files
 * could be handed the same block - and is reported loudly, though the
 * rebuild fixes it by construction.
 *
 * Deliberately not attempted: cross-checking directory entries against
 * inodes, which would find an inode nothing names. That needs somewhere
 * to put what it finds (a lost+found), and inventing one is a bigger
 * decision than this milestone should make quietly.
 */
static uint8_t check_bitmap[BITMAP_SECTORS * LEANFS_BLOCK_SIZE];

static void check_mark(uint32_t block) {
    if (block_valid(block)) {
        check_bitmap[block / 8] |= (uint8_t)(1u << (block % 8));
    }
}

void leanfs_check(void) {
    k_memset(check_bitmap, 0, sizeof(check_bitmap));
    /* Block 0 is the never-allocated sentinel and the bitmap has always
     * held it - see alloc_block. Marking it here keeps it from being
     * reported as an orphan on every single check. */
    check_mark(0);

    uint32_t table[LEANFS_INDIRECT_POINTERS];
    for (int idx = 0; idx < LEANFS_MAX_INODES; idx++) {
        leanfs_inode_t *inode = &inodes[idx];
        if (inode->type == LEANFS_TYPE_FREE) {
            continue;
        }
        uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
        for (uint32_t b = 0; b < nblocks; b++) {
            int64_t blk = map_block(idx, b, 0);
            if (blk >= 0) {
                check_mark((uint32_t)blk);
            }
        }
        if (block_present(inode->indirect)) {
            check_mark(inode->indirect);
        }
        if (block_present(inode->dindirect)) {
            check_mark(inode->dindirect);
            ata_read_sectors(sb.data_lba + inode->dindirect, 1, (uint8_t *)table);
            for (int i = 0; i < LEANFS_INDIRECT_POINTERS; i++) {
                if (block_present(table[i])) {
                    check_mark(table[i]);
                }
            }
        }
    }

    uint32_t orphans = 0, missing = 0;
    for (uint32_t b = 0; b < LEANFS_DATA_BLOCKS; b++) {
        int was_used = (bitmap[b / 8] >> (b % 8)) & 1u;
        int is_used = (check_bitmap[b / 8] >> (b % 8)) & 1u;
        if (was_used && !is_used) {
            orphans++;
        } else if (!was_used && is_used) {
            missing++;
        }
    }

    if (orphans || missing) {
        k_memcpy(bitmap, check_bitmap, sizeof(bitmap));
        mark_all_blocks();
        save_meta();
    }

    klog_puts("[fs] leanfs check: ");
    klog_put_dec(orphans);
    klog_puts(" orphaned block(s) reclaimed, ");
    klog_put_dec(missing);
    klog_puts(" block(s) were in use but marked free");
    klog_puts(orphans || missing ? " - bitmap rebuilt from the inodes\n" : " - nothing to fix\n");
}

static void free_inode_blocks(int idx) {
    leanfs_inode_t *inode = &inodes[idx];
    uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
    uint32_t table[LEANFS_INDIRECT_POINTERS];

    for (uint32_t b = 0; b < nblocks; b++) {
        int64_t blk = map_block(idx, b, 0);
        if (blk >= 0) {
            bitmap_clear((uint32_t)blk);
        }
    }
    if (block_present(inode->indirect)) {
        bitmap_clear(inode->indirect);
    }
    if (block_present(inode->dindirect)) {
        ata_read_sectors(sb.data_lba + inode->dindirect, 1, (uint8_t *)table);
        for (int i = 0; i < LEANFS_INDIRECT_POINTERS; i++) {
            if (block_present(table[i])) {
                bitmap_clear(table[i]);
            }
        }
        bitmap_clear(inode->dindirect);
    }
    for (int b = 0; b < LEANFS_DIRECT_BLOCKS; b++) {
        inode->direct[b] = 0;
    }
    inode->indirect = 0;
    inode->dindirect = 0;
    mark_inode(idx);
}

/* M59: byte-range read. The whole-file leanfs_read is this with off 0 and
 * maxlen the size - kept as its own name because every existing caller is
 * a small whole file and rewriting nine working callers would be scope
 * nothing has asked for. */
static int64_t inode_pread(int idx, void *buf, size_t len, uint32_t off) {
    leanfs_inode_t *inode = &inodes[idx];
    if (off >= inode->size) {
        return 0;
    }
    size_t avail = inode->size - off;
    if (len > avail) {
        len = avail;
    }
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    size_t copied = 0;
    while (copied < len) {
        uint32_t pos = off + (uint32_t)copied;
        int64_t blk = map_block(idx, pos / LEANFS_BLOCK_SIZE, 0);
        size_t within = pos % LEANFS_BLOCK_SIZE;
        size_t chunk = LEANFS_BLOCK_SIZE - within;
        if (chunk > len - copied) {
            chunk = len - copied;
        }
        if (blk < 0) {
            /* A hole. lseek past the end and write leaves them, and
             * reading one gives zeros - which is what every filesystem
             * that has sparse files does, and is cheaper than refusing. */
            k_memset((uint8_t *)buf + copied, 0, chunk);
        } else {
            ata_read_sectors(sb.data_lba + (uint32_t)blk, 1, block_buf);
            k_memcpy((uint8_t *)buf + copied, block_buf + within, chunk);
        }
        copied += chunk;
    }
    return (int64_t)copied;
}

/* M59: byte-range write, growing the file as needed. A partial-block
 * write reads the block first - the alternative is zeroing the bytes
 * around it, which is how a one-character edit eats the rest of a line. */
static int64_t inode_pwrite(int idx, const void *buf, size_t len, uint32_t off) {
    leanfs_inode_t *inode = &inodes[idx];
    if (off > (uint32_t)LEANFS_MAX_FILE_SIZE || len > (size_t)LEANFS_MAX_FILE_SIZE - off) {
        return -1;
    }
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    size_t written = 0;
    while (written < len) {
        uint32_t pos = off + (uint32_t)written;
        uint32_t logical = pos / LEANFS_BLOCK_SIZE;
        size_t within = pos % LEANFS_BLOCK_SIZE;
        size_t chunk = LEANFS_BLOCK_SIZE - within;
        if (chunk > len - written) {
            chunk = len - written;
        }
        int64_t blk = map_block(idx, logical, 1);
        if (blk < 0) {
            break; /* out of space - keep what landed, report it, see below */
        }
        if (chunk != LEANFS_BLOCK_SIZE) {
            /* Read the block back only if it already held part of this
             * file - and the question is about the *block*, not the
             * position. A block whose start is past the old end of file
             * is whatever the allocator handed over, which is very likely
             * some other file's freed data, so it is zeroed rather than
             * partially overwritten; a block that straddles the old end
             * still holds real bytes below it and must not be.
             *
             * Getting this wrong either way is a data bug rather than a
             * cosmetic one: zeroing too eagerly loses the bytes before
             * the write, and reading too eagerly leaks a deleted file's
             * contents into the tail of a new one. */
            uint32_t block_start = logical * (uint32_t)LEANFS_BLOCK_SIZE;
            if (block_start < inode->size) {
                ata_read_sectors(sb.data_lba + (uint32_t)blk, 1, block_buf);
            } else {
                k_memset(block_buf, 0, sizeof(block_buf));
            }
        }
        k_memcpy(block_buf + within, (const uint8_t *)buf + written, chunk);
        ata_write_sectors(sb.data_lba + (uint32_t)blk, 1,
                           chunk == LEANFS_BLOCK_SIZE ? (const uint8_t *)buf + written : block_buf);
        written += chunk;
        if (pos + chunk > inode->size) {
            inode->size = pos + (uint32_t)chunk;
            mark_inode(idx);
        }
    }
    if (written > 0) {
        inode->mtime = rtc_now();
        mark_inode(idx);
    }
    return written == 0 && len > 0 ? -1 : (int64_t)written;
}

static int64_t inode_read_data(int idx, void *buf, size_t maxlen) {
    int64_t n = inode_pread(idx, buf, maxlen, 0);
    if (n < 0) {
        return -1;
    }
    return (int64_t)inodes[idx].size;
}

/* Replaces an inode's entire contents. Frees whatever backed it first -
 * a fresh write always gets a fresh set of blocks, kept simple rather
 * than reusing them in place. Leaves the inode's own type alone, so this
 * serves files and directories identically. Does not save the tables;
 * the caller does that once, after whatever else it also changed. */
static int inode_write_data(int idx, const void *buf, size_t len) {
    if (len > (size_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    free_inode_blocks(idx);
    inodes[idx].size = 0;
    mark_inode(idx);
    if (len == 0) {
        inodes[idx].mtime = rtc_now();
        return 0;
    }
    if (inode_pwrite(idx, buf, len, 0) != (int64_t)len) {
        /* Out of space part way through. Everything allocated so far is
         * still reachable through the inode, so freeing it is a walk, not
         * a rollback log - and leaving it would be a leak of exactly the
         * blocks a full disk cannot spare. */
        free_inode_blocks(idx);
        inodes[idx].size = 0;
        mark_inode(idx);
        return -1;
    }
    return 0;
}

/* ---- directories ----------------------------------------------------- */

/* M81: a directory is a sequence of whole blocks, and each block is a
 * sequence of variable-length records that tile it exactly (see
 * leanfs_dirent_t in leanfs.h). Every operation below is therefore
 * "find the block, rewrite that block" - never "load the directory".
 *
 * The invariants, in one place, because four functions depend on all of
 * them and none of them is checked by the type system:
 *
 *   1. inodes[dir].size is a whole multiple of LEANFS_BLOCK_SIZE.
 *   2. Within a block, the rec_lens sum to exactly LEANFS_BLOCK_SIZE.
 *   3. Every rec_len is >= LEANFS_DIRENT_HDR, is a multiple of
 *      LEANFS_DIRENT_ALIGN, and is >= LEANFS_DIRENT_NEED(name_len).
 *   4. inode == 0 means free space; the name and type mean nothing.
 *
 * dir_block_valid checks 2 and 3 on every block this driver reads, and a
 * block that fails is treated as an unreadable directory rather than
 * walked - a corrupt rec_len is otherwise an unbounded loop or a read off
 * the end of the buffer, which is exactly the class of bug M29 added
 * block_valid to prevent on the block numbers.
 */

static leanfs_dirent_t *dir_rec(uint32_t off) {
    return (leanfs_dirent_t *)(dir_block + off);
}

/* Walks the block in `dir_block` and returns 1 if it satisfies invariants
 * 2 and 3, 0 otherwise. */
static int dir_block_valid(void) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        /* Bounds-check the header BEFORE reading it. This function is the
         * only one here that runs on bytes straight off the disk, so it is
         * the only one that can be handed an off that leaves no room for a
         * record header - and reading rec_len at 508 would read four bytes
         * past dir_block. Every other walk below runs after this one has
         * returned 1, which guarantees rec_len >= 8 and off + rec_len <=
         * 512, so no reachable off there can exceed 504. */
        if (off + LEANFS_DIRENT_HDR > LEANFS_BLOCK_SIZE) {
            return 0;
        }
        leanfs_dirent_t *r = dir_rec(off);
        if (r->rec_len < LEANFS_DIRENT_HDR ||
            (r->rec_len % LEANFS_DIRENT_ALIGN) != 0 ||
            off + r->rec_len > LEANFS_BLOCK_SIZE ||
            LEANFS_DIRENT_NEED(r->name_len) > r->rec_len) {
            return 0;
        }
        off += r->rec_len;
    }
    return off == LEANFS_BLOCK_SIZE;
}

/* Lays out an empty block: one free record covering the whole thing. */
static void dir_block_init(void) {
    k_memset(dir_block, 0, LEANFS_BLOCK_SIZE);
    dir_rec(0)->rec_len = (uint16_t)LEANFS_BLOCK_SIZE;
}

/* Reads directory block `logical` into dir_block. Returns 1 on success, 0
 * if the block is a hole (which a directory should never have, and which
 * is treated as an empty block rather than as an error so that a
 * half-grown directory still reads), -1 if it is unreadable or corrupt. */
static int dir_block_read(int idx, uint32_t logical) {
    int64_t blk = map_block(idx, logical, 0);
    if (blk < 0) {
        dir_block_init();
        return 0;
    }
    ata_read_sectors(sb.data_lba + (uint32_t)blk, 1, dir_block);
    if (!dir_block_valid()) {
        return -1;
    }
    return 1;
}

static int dir_block_write(int idx, uint32_t logical) {
    int64_t blk = map_block(idx, logical, 1);
    if (blk < 0) {
        return -1;
    }
    ata_write_sectors(sb.data_lba + (uint32_t)blk, 1, dir_block);
    inodes[idx].mtime = rtc_now();
    mark_inode(idx);
    return 0;
}

static uint32_t dir_nblocks(int idx) {
    return inodes[idx].size / LEANFS_BLOCK_SIZE;
}

static int dir_ok(int idx) {
    return inode_valid(idx) && inodes[idx].type == LEANFS_TYPE_DIR;
}

/* The offset of the record naming `name` in the block currently in
 * dir_block, or -1. Compares against name_len rather than a NUL, because
 * a record's name is not terminated on disk. */
static int32_t dir_block_find(const char *name, uint32_t name_len) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        leanfs_dirent_t *r = dir_rec(off);
        if (r->inode != 0 && r->name_len == name_len &&
            k_memcmp(dir_block + off + LEANFS_DIRENT_HDR, name, name_len) == 0) {
            return (int32_t)off;
        }
        off += r->rec_len;
    }
    return -1;
}

/* Merges every run of adjacent free records in dir_block into one.
 *
 * Without this, deleting alternate entries in a directory leaves it full
 * of holes too small to hold anything, and a name long enough to need two
 * of them adjacent would fail to be created in a directory that is mostly
 * empty. Called on every removal, which is the only thing that makes a
 * hole, so runs never get a chance to build up. */
static void dir_block_coalesce(void) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        leanfs_dirent_t *r = dir_rec(off);
        if (r->inode == 0) {
            uint32_t next = off + r->rec_len;
            while (next < LEANFS_BLOCK_SIZE && dir_rec(next)->inode == 0) {
                r->rec_len = (uint16_t)(r->rec_len + dir_rec(next)->rec_len);
                next = off + r->rec_len;
            }
            r->name_len = 0;
            r->type = 0;
        }
        off += dir_rec(off)->rec_len;
    }
}

/* Tries to place a record for `name` in the block currently in dir_block.
 * Returns 1 if it fitted (dir_block is now dirty and must be written), 0
 * if there was no room.
 *
 * Two kinds of room, and taking them in this order matters: a whole free
 * record big enough is used as-is, and otherwise a live record with slack
 * beyond what its own name needs is shrunk to its true size and the
 * remainder becomes the new record. Preferring free records means a
 * directory that has had entries removed reuses those holes before it
 * starts carving up the tail of a block. */
static int dir_block_place(const char *name, uint32_t name_len, int inode_idx) {
    uint32_t need = LEANFS_DIRENT_NEED(name_len);

    for (int pass = 0; pass < 2; pass++) {
        uint32_t off = 0;
        while (off < LEANFS_BLOCK_SIZE) {
            leanfs_dirent_t *r = dir_rec(off);
            uint32_t place_at = 0;
            uint32_t place_len = 0;

            if (pass == 0 && r->inode == 0 && r->rec_len >= need) {
                place_at = off;
                place_len = r->rec_len;
            } else if (pass == 1 && r->inode != 0) {
                uint32_t used = LEANFS_DIRENT_NEED(r->name_len);
                if (r->rec_len >= used + need) {
                    place_len = r->rec_len - used;
                    r->rec_len = (uint16_t)used;
                    place_at = off + used;
                }
            }

            if (place_len > 0) {
                leanfs_dirent_t *n = dir_rec(place_at);
                n->inode = (uint32_t)inode_idx;
                n->rec_len = (uint16_t)place_len;
                n->name_len = (uint8_t)name_len;
                n->type = (uint8_t)inodes[inode_idx].type;
                k_memcpy(dir_block + place_at + LEANFS_DIRENT_HDR, name, name_len);
                return 1;
            }
            off += r->rec_len;
        }
    }
    return 0;
}

/* The inode `name` refers to inside the directory `dir`, or -1. */
static int dir_lookup(int dir, const char *name) {
    if (!dir_ok(dir)) {
        return -1;
    }
    uint32_t name_len = (uint32_t)k_strlen(name);
    uint32_t blocks = dir_nblocks(dir);
    for (uint32_t b = 0; b < blocks; b++) {
        if (dir_block_read(dir, b) < 0) {
            return -1;
        }
        int32_t off = dir_block_find(name, name_len);
        if (off >= 0) {
            return (int)dir_rec((uint32_t)off)->inode;
        }
    }
    return -1;
}

/* Adds one record naming `inode_idx`. Reuses a hole in an existing block
 * before growing the directory by one, so a create/remove cycle does not
 * make a directory grow without bound. The record's type is read from the
 * inode rather than passed in - two sources of truth for what a thing is
 * is the bug M53 removed from this filesystem and there is no reason to
 * put it back. Returns 0 or -1. */
static int dir_add(int dir, const char *name, int inode_idx) {
    if (!dir_ok(dir) || !inode_valid(inode_idx)) {
        return -1;
    }
    uint32_t name_len = (uint32_t)k_strlen(name);
    if (name_len == 0 || name_len > LEANFS_MAX_NAME) {
        return -1;
    }

    /* M81: start where the last insert into this directory succeeded.
     *
     * Measured, not assumed. Without the hint this loop scans from block
     * zero every time, and each block scanned is a 512-byte PIO read - so
     * filling a directory is quadratic in the number of files, which is
     * precisely the workload this milestone exists to make possible.
     * Creating 1200 files in one directory took 18.9 s of a boot; with
     * the hint it is a single block read per insert until a block fills.
     *
     * The hint is a pure optimization and is never trusted: if the scan
     * from it finds nothing, the scan from zero runs anyway (below), so a
     * stale or wrong hint costs one extra pass and can never lose a hole.
     * That is why it is one entry rather than a table - a directory being
     * filled is the case worth catching, and being filled is something
     * one directory at a time. */
    uint32_t blocks = dir_nblocks(dir);
    uint32_t start = (dir == dir_hint_inode && dir_hint_block < blocks) ? dir_hint_block : 0;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t from = (pass == 0) ? start : 0;
        uint32_t to = (pass == 0) ? blocks : start;
        for (uint32_t b = from; b < to; b++) {
            if (dir_block_read(dir, b) < 0) {
                return -1;
            }
            if (dir_block_place(name, name_len, inode_idx)) {
                dir_hint_inode = dir;
                dir_hint_block = b;
                return dir_block_write(dir, b);
            }
        }
        if (start == 0) {
            break; /* the first pass already covered the whole directory */
        }
    }

    /* No room anywhere: grow by one block. The size only moves once the
     * block is safely written, so a failure to allocate leaves a
     * directory that is one block shorter rather than one block of
     * garbage longer. */
    if (inodes[dir].size > (uint32_t)LEANFS_MAX_FILE_SIZE - LEANFS_BLOCK_SIZE) {
        return -1;
    }
    dir_block_init();
    if (!dir_block_place(name, name_len, inode_idx)) {
        return -1; /* cannot happen: an empty block holds the longest name */
    }
    if (dir_block_write(dir, blocks) != 0) {
        return -1;
    }
    dir_hint_inode = dir;
    dir_hint_block = blocks;
    inodes[dir].size += LEANFS_BLOCK_SIZE;
    mark_inode(dir);
    return 0;
}

/* M71: point an EXISTING directory entry at a different inode, in place.
 *
 * The difference from remove-then-add is the only thing that matters
 * here: this changes one record from one inode number to another, so
 * there is no instant at which the name does not resolve. That is what
 * makes write-temp-then-rename an atomic replace rather than a slightly
 * shorter version of the same window SYS_writefile has always had.
 *
 * Returns 0 if the name existed and now points at `inode_idx`, -1 if
 * there was no such entry (which is not an error to the caller - it means
 * "this is a plain rename into a free name", and dir_add handles that). */
static int dir_repoint(int dir, const char *name, int inode_idx) {
    if (!dir_ok(dir) || !inode_valid(inode_idx)) {
        return -1;
    }
    uint32_t name_len = (uint32_t)k_strlen(name);
    uint32_t blocks = dir_nblocks(dir);
    for (uint32_t b = 0; b < blocks; b++) {
        if (dir_block_read(dir, b) < 0) {
            return -1;
        }
        int32_t off = dir_block_find(name, name_len);
        if (off >= 0) {
            leanfs_dirent_t *r = dir_rec((uint32_t)off);
            r->inode = (uint32_t)inode_idx;
            r->type = (uint8_t)inodes[inode_idx].type;
            return dir_block_write(dir, b);
        }
    }
    return -1;
}

/* ---- paths ----------------------------------------------------------- */

/* Copies the next '/'-delimited component of *p into out (at most
 * LEANFS_MAX_NAME chars plus NUL) and advances *p past it. Returns 1 if a
 * component was taken, 0 at the end of the path, -1 for a malformed one -
 * an empty component ("//" or a trailing slash on a non-root path), one
 * that is too long, or "." / ".." which this format deliberately does not
 * store and therefore cannot honestly resolve. */
static int next_component(const char **p, char *out) {
    const char *s = *p;
    if (*s == '\0') {
        return 0;
    }
    int n = 0;
    while (*s && *s != '/') {
        if (n >= LEANFS_MAX_NAME) {
            return -1;
        }
        out[n++] = *s++;
    }
    out[n] = '\0';
    if (n == 0) {
        return -1; /* "//" or a trailing '/' */
    }
    if (out[0] == '.' && (out[1] == '\0' || (out[1] == '.' && out[2] == '\0'))) {
        return -1; /* no "." or ".." on disk to resolve against - see the header */
    }
    if (*s == '/') {
        s++;
        /* M60: a single trailing slash is now accepted, and the reason is
         * concrete rather than a change of taste. This refused "/bin/"
         * outright, which was defensible while nothing produced such a
         * path - and M60's tab completion produces one every time it
         * completes a directory, because appending '/' is what tells you
         * it is one and lets the next Tab descend into it. "/bin/" names
         * exactly the same directory "/bin" does; there is nothing
         * ambiguous to refuse.
         *
         * "//" is still refused, by the empty-component check above: this
         * only steps past the separator, and the next call sees the end
         * of the string (returning 0) or another '/' (returning -1). And
         * resolve_parent still refuses a trailing slash of its own,
         * because there a trailing slash means the caller is naming a
         * *leaf* and has not said what it is called. */
    }
    *p = s;
    return 1;
}

/* Resolves an absolute path to an inode index, or -1.
 *
 * Deliberately no "." or ".." support: neither is stored on disk, and
 * synthesizing them would mean either walking a parent pointer this
 * format does not have or rewriting the path textually, which is the kind
 * of near-correct shortcut that turns into an escape from the root. The
 * file manager's own ".." is a caller-side string operation on a path it
 * already holds, which is honest about being exactly that. */
static int resolve(const char *path) {
    if (!path || path[0] != '/') {
        return -1;
    }
    if (path[1] == '\0') {
        return ROOT_INODE;
    }
    const char *p = path + 1;
    int at = ROOT_INODE;
    char comp[LEANFS_MAX_NAME + 1];
    int rc;
    while ((rc = next_component(&p, comp)) == 1) {
        if (inodes[at].type != LEANFS_TYPE_DIR) {
            return -1; /* tried to walk through a regular file */
        }
        at = dir_lookup(at, comp);
        if (!inode_valid(at)) {
            return -1;
        }
    }
    if (rc < 0) {
        return -1;
    }
    /* M60: a trailing slash is a claim that the thing named is a
     * directory, so it is honoured for one and refused for a file.
     * "/bin/" and "/bin" name the same directory - which is what makes
     * tab completion's directory suffix usable - but "/bin/ls/" is
     * saying something untrue about `ls`, and a resolver that shrugged
     * at that would let a caller act on a file it believed was a
     * directory. */
    size_t len = k_strlen(path);
    if (len > 1 && path[len - 1] == '/' && inodes[at].type != LEANFS_TYPE_DIR) {
        return -1;
    }
    return at;
}

/* Splits an absolute path into its parent directory's inode and the leaf
 * name. Returns 0 with *out_parent and out_leaf filled, or -1. The root
 * itself has no parent and is refused. */
static int resolve_parent(const char *path, int *out_parent, char *out_leaf) {
    if (!path || path[0] != '/' || path[1] == '\0') {
        return -1;
    }
    /* The leaf is whatever follows the last '/'. Finding it first means
     * the walk below only ever has to handle interior components, which
     * is what keeps next_component's rules ("no empty, no trailing
     * slash") uniform. */
    const char *last = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/') {
            last = s;
        }
    }
    if (last[1] == '\0') {
        return -1; /* trailing slash */
    }
    int n = 0;
    for (const char *s = last + 1; *s; s++) {
        if (n >= LEANFS_MAX_NAME) {
            return -1;
        }
        out_leaf[n++] = *s;
    }
    out_leaf[n] = '\0';
    if (out_leaf[0] == '.' && (out_leaf[1] == '\0' || (out_leaf[1] == '.' && out_leaf[2] == '\0'))) {
        return -1;
    }

    int parent;
    if (last == path) {
        parent = ROOT_INODE; /* "/name" */
    } else {
        char dir_path[LEANFS_MAX_PATH];
        size_t dir_len = (size_t)(last - path);
        if (dir_len >= sizeof(dir_path)) {
            return -1;
        }
        k_memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        parent = resolve(dir_path);
    }
    if (!inode_valid(parent) || inodes[parent].type != LEANFS_TYPE_DIR) {
        return -1;
    }
    *out_parent = parent;
    return 0;
}

/* M56: drops the record naming `name` from `dir`. Returns the inode the
 * record pointed at, or -1.
 *
 * M81: the record is marked free (inode 0) and merged with any free
 * neighbours rather than the block being compacted - which is what keeps
 * a removal to one block write, and what lets dir_add reuse the hole. A
 * directory therefore never shrinks; it only stops growing. That is the
 * same trade every filesystem of this shape makes, and the alternative -
 * moving records between blocks to close a gap - would invalidate the
 * byte offsets leanfs_readdir hands out as cookies. */
static int dir_remove(int dir, const char *name) {
    if (!dir_ok(dir)) {
        return -1;
    }
    uint32_t name_len = (uint32_t)k_strlen(name);
    uint32_t blocks = dir_nblocks(dir);
    for (uint32_t b = 0; b < blocks; b++) {
        if (dir_block_read(dir, b) < 0) {
            return -1;
        }
        int32_t off = dir_block_find(name, name_len);
        if (off >= 0) {
            leanfs_dirent_t *r = dir_rec((uint32_t)off);
            int idx = (int)r->inode;
            r->inode = 0;
            r->name_len = 0;
            r->type = 0;
            dir_block_coalesce();
            /* This block now has room, so no later insert may skip past
             * it. Lowering rather than clearing keeps the hint useful for
             * the directory being filled; taking it over outright when it
             * belongs to a different directory is right too, because the
             * one with a fresh hole is the better guess. */
            if (dir != dir_hint_inode || b < dir_hint_block) {
                dir_hint_inode = dir;
                dir_hint_block = b;
            }
            return dir_block_write(dir, b) == 0 ? idx : -1;
        }
    }
    return -1;
}

/* M81: how many live entries a directory holds. Only ever compared
 * against zero (rmdir), so it stops at the first one it finds rather than
 * reading every block of a directory to answer a yes/no question. */
static int dir_is_empty(int idx) {
    if (!dir_ok(idx)) {
        return 0;
    }
    uint32_t blocks = dir_nblocks(idx);
    for (uint32_t b = 0; b < blocks; b++) {
        if (dir_block_read(idx, b) < 0) {
            return 0;
        }
        uint32_t off = 0;
        while (off < LEANFS_BLOCK_SIZE) {
            if (dir_rec(off)->inode != 0) {
                return 0;
            }
            off += dir_rec(off)->rec_len;
        }
    }
    return 1;
}

/* ---- public API ------------------------------------------------------ */

int leanfs_exists(const char *path) {
    return inode_valid(resolve(path));
}

int leanfs_is_dir(const char *path) {
    int idx = resolve(path);
    return inode_valid(idx) && inodes[idx].type == LEANFS_TYPE_DIR;
}

int64_t leanfs_read(const char *path, void *buf, size_t maxlen) {
    int idx = resolve(path);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    return inode_read_data(idx, buf, maxlen);
}

int leanfs_write(const char *path, const void *buf, size_t len) {
    if (len > LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }

    int idx = dir_lookup(parent, leaf);
    if (inode_valid(idx)) {
        if (inodes[idx].type != LEANFS_TYPE_FILE) {
            return -1; /* overwriting a directory with a file is not a thing this format offers */
        }
    } else {
        idx = find_free_inode();
        if (idx < 0) {
            return -1;
        }
        k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
        inodes[idx].type = LEANFS_TYPE_FILE;
        mark_inode(idx);
        /* The record goes in *before* the contents, deliberately: dir_add
         * uses the same block machinery inode_write_data does, and doing
         * it afterwards would mean a failure to grow the directory left a
         * fully written file that nothing could name. */
        if (dir_add(parent, leaf, idx) != 0) {
            inodes[idx].type = LEANFS_TYPE_FREE;
            return -1;
        }
    }

    if (inode_write_data(idx, buf, len) != 0) {
        return -1;
    }
    save_meta();
    return 0;
}

int leanfs_mkdir(const char *path) {
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    if (inode_valid(dir_lookup(parent, leaf))) {
        return -1; /* already there, of either kind - see the header on why this isn't a no-op */
    }
    int idx = find_free_inode();
    if (idx < 0) {
        return -1;
    }
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    inodes[idx].type = LEANFS_TYPE_DIR;
    inodes[idx].size = 0;
    mark_inode(idx);
    if (dir_add(parent, leaf, idx) != 0) {
        inodes[idx].type = LEANFS_TYPE_FREE;
        mark_inode(idx);
        return -1;
    }
    save_meta();
    return 0;
}

uint32_t leanfs_free_blocks(void) {
    uint32_t free_count = 0;
    for (uint32_t i = 0; i < sb.data_blocks; i++) {
        if (!bitmap_test(i)) {
            free_count++;
        }
    }
    return free_count;
}

int leanfs_unlink(const char *path) {
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    int idx = dir_lookup(parent, leaf);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    /* Blocks first, then the inode, then the name - each step making the
     * thing before it unreachable, so an interruption anywhere leaves a
     * name pointing at a free inode (which resolve() refuses) rather than
     * a live inode pointing at blocks somebody else now owns. */
    free_inode_blocks(idx);
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    mark_inode(idx);
    if (dir_remove(parent, leaf) < 0) {
        return -1;
    }
    save_meta();
    return 0;
}

int leanfs_rename(const char *old_path, const char *new_path) {
    int old_parent, new_parent;
    char old_leaf[LEANFS_MAX_NAME + 1];
    char new_leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(old_path, &old_parent, old_leaf) != 0 ||
        resolve_parent(new_path, &new_parent, new_leaf) != 0) {
        return -1;
    }
    int idx = dir_lookup(old_parent, old_leaf);
    if (!inode_valid(idx)) {
        return -1;
    }
    if (inode_valid(dir_lookup(new_parent, new_leaf))) {
        return -1; /* taken - see the header on why this isn't a silent replace */
    }
    /* Added before removed, deliberately. Both halves rewrite a directory
     * through the same block allocator a file uses, so either can fail on
     * a full disk - and the order that survives a failure is the one
     * where the file still has *a* name rather than none at all. A
     * duplicate name is recoverable; an unreachable inode is not. */
    if (dir_add(new_parent, new_leaf, idx) != 0) {
        return -1;
    }
    if (dir_remove(old_parent, old_leaf) < 0) {
        return -1;
    }
    save_meta();
    return 0;
}

/* M71: simulate, exactly, what a crash between "allocate the blocks" and
 * "record who owns them" leaves behind.
 *
 * Removes the directory entry and frees the INODE, but deliberately does
 * NOT free the blocks - so the bitmap still says they are in use and
 * nothing in the filesystem refers to them any more. That is the leak,
 * and it is the one a crash actually produces here: leanfs is
 * write-through with a single writer, so what a power cut interrupts is
 * not a torn transaction but the sequence of separate writes that make up
 * an allocation.
 *
 * A debug hook rather than a real operation, in the same spirit as M66's
 * tcp_debug_drop_next: the failure being tested is one the machine
 * cannot be asked to produce on demand, so it is produced honestly here
 * rather than approximated by a test that checks something easier. */
void leanfs_debug_orphan(const char *path) {
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return;
    }
    int idx = dir_lookup(parent, leaf);
    if (!inode_valid(idx)) {
        return;
    }
    dir_remove(parent, leaf);
    inodes[idx].type = LEANFS_TYPE_FREE;
    inodes[idx].size = 0;
    for (int b = 0; b < LEANFS_DIRECT_BLOCKS; b++) {
        inodes[idx].direct[b] = 0;
    }
    inodes[idx].indirect = 0;
    inodes[idx].dindirect = 0;
    mark_inode(idx);
    save_meta();
}

/* M71: see leanfs.h. */
void leanfs_sync(void) {
    sb.state = LEANFS_STATE_CLEAN;
    save_superblock();
}

int leanfs_rename_replace(const char *old_path, const char *new_path) {
    int old_parent, new_parent;
    char old_leaf[LEANFS_MAX_NAME + 1];
    char new_leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(old_path, &old_parent, old_leaf) != 0 ||
        resolve_parent(new_path, &new_parent, new_leaf) != 0) {
        return -1;
    }
    int idx = dir_lookup(old_parent, old_leaf);
    if (!inode_valid(idx)) {
        return -1;
    }
    int victim = dir_lookup(new_parent, new_leaf);
    if (inode_valid(victim)) {
        if (inodes[victim].type == LEANFS_TYPE_DIR) {
            return -1; /* replacing a directory with a file is not a rename, it is a mistake */
        }
        if (victim == idx) {
            return 0; /* renaming a file onto itself - nothing to do, and unlinking would lose it */
        }
    }

    /* THE ORDER IS THE WHOLE POINT, and it is the opposite of
     * leanfs_rename's.
     *
     * The dance this exists for is: write the new contents to a temp
     * name, then rename it over the real one. What must never happen is
     * a moment where the real name does not resolve - that is precisely
     * the window in which a crash loses the document, and it is the
     * window SYS_writefile's truncate-then-write has always had.
     *
     * So the directory entry is REPOINTED rather than removed and re-
     * added: one record changes from the old inode to the new one, and
     * there is no instant at which the name is absent. The old inode is
     * released afterwards, because a leaked inode is recoverable (M71's
     * own check reclaims its blocks) and a missing file is not. */
    if (dir_repoint(new_parent, new_leaf, idx) != 0) {
        /* No existing entry to repoint - this is a plain rename into a
         * free name, which is what dir_add is for. */
        if (dir_add(new_parent, new_leaf, idx) != 0) {
            return -1;
        }
    } else if (inode_valid(victim)) {
        free_inode_blocks(victim);
        inodes[victim].type = LEANFS_TYPE_FREE;
        inodes[victim].size = 0;
        mark_inode(victim);
    }
    if (dir_remove(old_parent, old_leaf) < 0) {
        return -1;
    }
    save_meta();
    return 0;
}

int leanfs_rmdir(const char *path) {
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    int idx = dir_lookup(parent, leaf);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_DIR) {
        return -1;
    }
    /* Empty only. Recursive delete is one keystroke away from losing
     * everything under a path and this OS has no trash to take it back
     * out of - the same judgment leanfs_unlink makes when it refuses a
     * directory rather than guessing what was meant. */
    if (!dir_is_empty(idx)) {
        return -1;
    }
    free_inode_blocks(idx);
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    mark_inode(idx);
    if (dir_remove(parent, leaf) < 0) {
        return -1;
    }
    save_meta();
    return 0;
}

int leanfs_stat(const char *path, leanfs_stat_t *out) {
    int idx = resolve(path);
    if (!inode_valid(idx)) {
        return -1;
    }
    out->size = inodes[idx].size;
    out->mtime = inodes[idx].mtime;
    out->is_dir = inodes[idx].type == LEANFS_TYPE_DIR;
    return 0;
}

int leanfs_handle_stat(int handle, leanfs_stat_t *out) {
    if (!inode_valid(handle)) {
        return -1;
    }
    out->size = inodes[handle].size;
    out->mtime = inodes[handle].mtime;
    out->is_dir = inodes[handle].type == LEANFS_TYPE_DIR;
    return 0;
}

/* ---- M59: descriptors ------------------------------------------------ */

int leanfs_open(const char *path, int create) {
    int idx = resolve(path);
    if (inode_valid(idx)) {
        /* M87: `create` carrying LEANFS_OPEN_EXCL means the caller is
         * asking to be the one who made this file, not merely to have it
         * open. Existing is the failure it is asking about.
         *
         * This is what makes a lock file a lock: two processes that both
         * do it, and exactly one succeeds. <fcntl.h> defined O_EXCL as 0
         * with a comment saying "a program that relies on O_EXCL to avoid
         * a race gets no protection" - it does now. The atomicity is the
         * whole point and it comes from fs_lock: the resolve and the
         * create below happen inside one critical section with interrupts
         * off, so there is no instant between them for a second caller to
         * slip into. */
        if (create & LEANFS_OPEN_EXCL) {
            return -1;
        }
        return inodes[idx].type == LEANFS_TYPE_FILE ? idx : -1;
    }
    if (!create) {
        return -1;
    }
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    idx = find_free_inode();
    if (idx < 0) {
        return -1;
    }
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    inodes[idx].type = LEANFS_TYPE_FILE;
    inodes[idx].mtime = rtc_now();
    mark_inode(idx);
    /* The record goes in before any contents can, exactly as
     * leanfs_write does it and for the same reason: a file nothing can
     * name is a leak this filesystem has no way to find again. */
    if (dir_add(parent, leaf, idx) != 0) {
        inodes[idx].type = LEANFS_TYPE_FREE;
        mark_inode(idx);
        return -1;
    }
    save_meta();
    return idx;
}

int64_t leanfs_handle_read(int handle, void *buf, size_t len, uint32_t off) {
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    return inode_pread(handle, buf, len, off);
}

int64_t leanfs_handle_write(int handle, const void *buf, size_t len, uint32_t off) {
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    int64_t n = inode_pwrite(handle, buf, len, off);
    if (n > 0) {
        save_meta();
    }
    return n;
}

uint32_t leanfs_handle_size(int handle) {
    if (!inode_valid(handle)) {
        return 0;
    }
    return inodes[handle].size;
}

/* M87: forget one logical block, so a truncated file does not keep a
 * pointer to a block the allocator has handed to somebody else. The
 * mirror of map_block's allocate path, and deliberately only the direct
 * and single-indirect cases plus the double: the same three the mapper
 * knows about, in the same order, so the two cannot disagree about where
 * a block number lives. */
static void clear_block_pointer(int idx, uint32_t logical) {
    leanfs_inode_t *inode = &inodes[idx];
    uint32_t table[LEANFS_INDIRECT_POINTERS];

    if (logical < LEANFS_DIRECT_BLOCKS) {
        inode->direct[logical] = 0;
        mark_inode(idx);
        return;
    }
    logical -= LEANFS_DIRECT_BLOCKS;
    if (logical < (uint32_t)LEANFS_INDIRECT_POINTERS) {
        if (!block_present(inode->indirect)) {
            return;
        }
        ata_read_sectors(sb.data_lba + inode->indirect, 1, (uint8_t *)table);
        table[logical] = 0;
        ata_write_sectors(sb.data_lba + inode->indirect, 1, (const uint8_t *)table);
        return;
    }
    logical -= (uint32_t)LEANFS_INDIRECT_POINTERS;
    if (!block_present(inode->dindirect)) {
        return;
    }
    uint32_t outer = logical / (uint32_t)LEANFS_INDIRECT_POINTERS;
    uint32_t inner = logical % (uint32_t)LEANFS_INDIRECT_POINTERS;
    if (outer >= (uint32_t)LEANFS_INDIRECT_POINTERS) {
        return;
    }
    ata_read_sectors(sb.data_lba + inode->dindirect, 1, (uint8_t *)table);
    uint32_t mid = table[outer];
    if (!block_present(mid)) {
        return;
    }
    ata_read_sectors(sb.data_lba + mid, 1, (uint8_t *)table);
    table[inner] = 0;
    ata_write_sectors(sb.data_lba + mid, 1, (const uint8_t *)table);
}

int leanfs_handle_truncate(int handle) {
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    free_inode_blocks(handle);
    inodes[handle].size = 0;
    inodes[handle].mtime = rtc_now();
    mark_inode(handle);
    save_meta();
    return 0;
}

/* M87: truncate to any length, not only to zero.
 *
 * Shrinking frees the blocks past the new end. Growing does nothing but
 * change the size, and that is not a shortcut - inode_pread already
 * returns zeros for a block that was never allocated (M59 called it "a
 * hole", and every filesystem with sparse files does the same), so a
 * file extended this way reads as zeros and costs nothing until
 * something writes into it. That is precisely what a program calling
 * ftruncate to reserve space expects, and on this machine it is also
 * what it gets: reserved, not allocated.
 *
 * One honest imperfection: an indirect table that becomes empty is left
 * allocated. Finding and freeing it means knowing whether every pointer
 * in it is now zero, which is a second walk to reclaim one block out of
 * sixteen thousand, and leanfs_check would not report it because the
 * inode still points at it legitimately. It comes back when the file
 * does. */
int leanfs_handle_truncate_to(int handle, uint32_t len) {
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    if (len > (uint32_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    uint32_t old = inodes[handle].size;
    if (len < old) {
        uint32_t first = (len + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
        uint32_t last = (old + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
        for (uint32_t b = first; b < last; b++) {
            int64_t blk = map_block(handle, b, 0);
            if (blk >= 0) {
                bitmap_clear((uint32_t)blk);
                clear_block_pointer(handle, b);
            }
        }
    }
    inodes[handle].size = len;
    inodes[handle].mtime = rtc_now();
    mark_inode(handle);
    save_meta();
    return 0;
}

/* M81: the one directory walk, which both public listing calls use.
 *
 * `*cookie` is a byte offset into the directory file. The block it lands
 * in is always walked from its start rather than indexed into directly,
 * which costs at most 64 header reads and buys two things: a cookie that
 * has been corrupted or handed back out of order can only ever land on a
 * real record boundary, and the caller never has to know that records are
 * variable-length. Returns 1 and fills `out`, 0 at the end, -1 if a block
 * is corrupt. */
static int dir_next(int idx, uint32_t *cookie, leanfs_dir_entry_t *out) {
    uint32_t blocks = dir_nblocks(idx);
    uint32_t pos = *cookie;

    while (pos / LEANFS_BLOCK_SIZE < blocks) {
        uint32_t b = pos / LEANFS_BLOCK_SIZE;
        uint32_t want = pos % LEANFS_BLOCK_SIZE;
        if (dir_block_read(idx, b) < 0) {
            return -1;
        }
        uint32_t off = 0;
        while (off < LEANFS_BLOCK_SIZE) {
            leanfs_dirent_t *r = dir_rec(off);
            if (off >= want && r->inode != 0) {
                uint32_t n = r->name_len;
                if (n > LEANFS_MAX_NAME) {
                    return -1;
                }
                out->inode = r->inode;
                out->is_dir = (uint8_t)(r->type == LEANFS_TYPE_DIR);
                k_memcpy(out->name, dir_block + off + LEANFS_DIRENT_HDR, n);
                out->name[n] = '\0';
                *cookie = b * LEANFS_BLOCK_SIZE + off + r->rec_len;
                return 1;
            }
            off += r->rec_len;
        }
        pos = (b + 1) * LEANFS_BLOCK_SIZE;
    }
    *cookie = blocks * LEANFS_BLOCK_SIZE;
    return 0;
}

int leanfs_dir_open(const char *path) {
    int idx = resolve(path);
    return dir_ok(idx) ? idx : -1;
}

int leanfs_readdir_at(int handle, uint32_t *cookie, leanfs_dir_entry_t *out) {
    /* Re-checked on every call rather than trusted from leanfs_dir_open.
     * A handle is an inode index (the same contract leanfs_open has had
     * since M59), so a directory removed between two calls leaves this
     * one naming a free or reused inode - and dir_ok is what turns that
     * into a clean -1 instead of a walk through whatever is there now. */
    if (!dir_ok(handle)) {
        return -1;
    }
    return dir_next(handle, cookie, out);
}

int leanfs_readdir(const char *path, uint32_t *cookie, leanfs_dir_entry_t *out) {
    int idx = leanfs_dir_open(path);
    if (idx < 0) {
        return -1;
    }
    return dir_next(idx, cookie, out);
}

size_t leanfs_list(const char *path, char *buf, size_t maxlen) {
    int idx = resolve(path);
    if (!dir_ok(idx)) {
        return 0;
    }
    /* M81: written over dir_next rather than over its own copy of the
     * walk. The '/' suffix is still built from the *inode's* type rather
     * than the record's, which is not redundancy for its own sake: the
     * record's type is what M77's d_type reports and this is the older
     * caller that predates it, so keeping the two answers coming from two
     * places is what makes the boot self-test's "d_type and st_mode
     * agree" assertion mean something. */
    size_t written = 0;
    uint32_t cookie = 0;
    leanfs_dir_entry_t e;
    while (dir_next(idx, &cookie, &e) == 1) {
        int child = (int)e.inode;
        int is_dir = inode_valid(child) && inodes[child].type == LEANFS_TYPE_DIR;
        size_t name_len = k_strlen(e.name);
        size_t need = name_len + (is_dir ? 1u : 0u) + 1u; /* name + optional '/' + '\n' */
        if (written + need > maxlen) {
            break;
        }
        k_memcpy(buf + written, e.name, name_len);
        written += name_len;
        if (is_dir) {
            buf[written++] = '/';
        }
        buf[written++] = '\n';
    }
    return written;
}
