#include "leanfs.h"

#include "drivers/ata.h"
#include "drivers/klog.h"
#include "drivers/rtc.h"
#include "lib/libk.h"

#define LEANFS_MAGIC     0x3353464Cu /* M59: bumped again (from M53's 0x3253464C, itself from M12's 0x3153464C). The inode grew an mtime and a double-indirect pointer, which moves every field after them - an old disk read with this layout would resolve garbage block numbers, so it is reformatted rather than misread. Nothing here has ever had a migration path and nothing on these disks has ever been worth one. */
#define LEANFS_START_LBA 2048u /* 1 MiB in - generously past the boot image; see Makefile's build-time size guard */

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
    uint32_t reserved;
} leanfs_superblock_t;

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
} leanfs_inode_t;

#define INODE_TABLE_SECTORS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_SECTORS       (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE)

/* The claim leanfs.h makes about records never straddling a block, as a
 * check the compiler makes rather than a comment somebody has to keep
 * true - a padded record would silently break every directory on disk. */
_Static_assert(sizeof(leanfs_dirent_t) == 32, "leanfs_dirent_t must stay 32 bytes so 16 fit exactly in a block");
_Static_assert(LEANFS_BLOCK_SIZE % sizeof(leanfs_dirent_t) == 0, "a directory record must not straddle a block");

static leanfs_superblock_t sb;
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[BITMAP_SECTORS * LEANFS_BLOCK_SIZE];

/* M53: static, not stack. A kernel task stack is 8 KiB (sched.c's
 * TASK_STACK_SIZE) and the inode table is now 15 sectors - 7680 bytes -
 * so the buffer these two used to declare locally would have overflowed
 * the stack of any user task that reached them through SYS_writefile.
 * It was already 3584 bytes before this milestone, which was uncomfortably
 * close to the same cliff without anyone having measured it. leanfs has
 * no concurrency of its own (every caller is inside a syscall, and the
 * scheduler is cooperative at these points), so one shared scratch buffer
 * is safe as well as smaller. */
static uint8_t inode_table_buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
/* One directory's whole contents, unpacked. Shared for the same reason,
 * and only ever live inside one directory operation at a time. */
static leanfs_dirent_t dirent_scratch[LEANFS_MAX_DIRENTS];

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

/* Writes only what changed. Runs of adjacent dirty sectors go out as one
 * ata_write_sectors call, because a run is exactly as cheap as a single
 * sector to set up and this is the path a whole-file write takes. */
static void save_meta(void) {
    k_memset(inode_table_buf, 0, sizeof(inode_table_buf));
    k_memcpy(inode_table_buf, inodes, sizeof(inodes));

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
        ata_write_sectors(sb.inode_table_lba + (uint32_t)i, (uint8_t)run,
                           inode_table_buf + i * LEANFS_BLOCK_SIZE);
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
        ata_write_sectors(sb.bitmap_lba + (uint32_t)i, (uint8_t)run,
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
    sb.reserved = 0;

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
     * entries needs no storage, and dir_store below allocates on demand. */
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
    } else {
        ata_read_sectors(sb.inode_table_lba, (uint8_t)sb.inode_table_sectors, inode_table_buf);
        k_memcpy(inodes, inode_table_buf, sizeof(inodes));
        ata_read_sectors(sb.bitmap_lba, (uint8_t)sb.bitmap_sectors, bitmap);

        /* A filesystem whose root is not a directory is one nothing can
         * be resolved against - reformat rather than fail every path. */
        if (inodes[ROOT_INODE].type != LEANFS_TYPE_DIR) {
            klog_puts("[fs] leanfs root inode is not a directory - reformatting\n");
            format();
        }
    }

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

/* Unpacks a directory's records into dirent_scratch and returns how many
 * slots it holds (including free ones, which keeps a slot's index stable
 * across a load/modify/store round trip). -1 if idx isn't a directory or
 * its contents are unreadable. */
static int dir_load(int idx) {
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_DIR) {
        return -1;
    }
    uint32_t bytes = inodes[idx].size;
    if (bytes > sizeof(dirent_scratch)) {
        return -1; /* more records than there are inodes to name - corrupt */
    }
    k_memset(dirent_scratch, 0, sizeof(dirent_scratch));
    if (bytes > 0 && inode_read_data(idx, dirent_scratch, bytes) < 0) {
        return -1;
    }
    return (int)(bytes / sizeof(leanfs_dirent_t));
}

static int dir_store(int idx, int count) {
    return inode_write_data(idx, dirent_scratch, (size_t)count * sizeof(leanfs_dirent_t));
}

/* The inode `name` refers to inside the directory `dir`, or -1. */
static int dir_lookup(int dir, const char *name) {
    int count = dir_load(dir);
    if (count < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (dirent_scratch[i].name[0] && k_strcmp(dirent_scratch[i].name, name) == 0) {
            return (int)dirent_scratch[i].inode;
        }
    }
    return -1;
}

/* Adds one record. Reuses a slot left free by a removal before growing
 * the directory, so a create/remove cycle doesn't make a directory grow
 * without bound. Returns 0 or -1. */
static int dir_add(int dir, const char *name, int inode_idx) {
    int count = dir_load(dir);
    if (count < 0) {
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < count; i++) {
        if (!dirent_scratch[i].name[0]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (count >= LEANFS_MAX_DIRENTS) {
            return -1;
        }
        slot = count++;
    }
    k_memset(&dirent_scratch[slot], 0, sizeof(dirent_scratch[slot]));
    k_strlcpy(dirent_scratch[slot].name, name, sizeof(dirent_scratch[slot].name));
    dirent_scratch[slot].inode = (uint32_t)inode_idx;
    return dir_store(dir, count);
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

/* M56: drops the record naming `name` from `dir`. The slot is blanked
 * rather than the array compacted, so every other entry keeps its index
 * across the round trip and dir_add can reuse the hole. Returns the
 * inode the record pointed at, or -1. */
static int dir_remove(int dir, const char *name) {
    int count = dir_load(dir);
    if (count < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (dirent_scratch[i].name[0] && k_strcmp(dirent_scratch[i].name, name) == 0) {
            int idx = (int)dirent_scratch[i].inode;
            k_memset(&dirent_scratch[i], 0, sizeof(dirent_scratch[i]));
            return dir_store(dir, count) == 0 ? idx : -1;
        }
    }
    return -1;
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
    int count = dir_load(idx);
    if (count < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (dirent_scratch[i].name[0]) {
            return -1;
        }
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

/* ---- M59: descriptors ------------------------------------------------ */

int leanfs_open(const char *path, int create) {
    int idx = resolve(path);
    if (inode_valid(idx)) {
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

size_t leanfs_list(const char *path, char *buf, size_t maxlen) {
    int idx = resolve(path);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_DIR) {
        return 0;
    }
    int count = dir_load(idx);
    if (count < 0) {
        return 0;
    }
    /* dir_load's results live in the shared scratch buffer, and the
     * is-it-a-directory question below re-reads `inodes` rather than the
     * scratch - so nothing here can invalidate what it is iterating. */
    size_t written = 0;
    for (int i = 0; i < count; i++) {
        if (!dirent_scratch[i].name[0]) {
            continue;
        }
        int child = (int)dirent_scratch[i].inode;
        int is_dir = inode_valid(child) && inodes[child].type == LEANFS_TYPE_DIR;
        size_t name_len = k_strlen(dirent_scratch[i].name);
        size_t need = name_len + (is_dir ? 1u : 0u) + 1u; /* name + optional '/' + '\n' */
        if (written + need > maxlen) {
            break;
        }
        k_memcpy(buf + written, dirent_scratch[i].name, name_len);
        written += name_len;
        if (is_dir) {
            buf[written++] = '/';
        }
        buf[written++] = '\n';
    }
    return written;
}
