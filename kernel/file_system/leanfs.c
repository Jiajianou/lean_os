#include "leanfs.h"

#include "drivers/block_device.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "panic.h"
#include "drivers/kernel_log.h"
#include "drivers/rtc.h"
#include "library/kernel_library.h"

#include "leanfs_format.h"

static void leanfs_forget_holds(void);

static leanfs_superblock_t sb;

static leanfs_inode_t *inodes;
static uint8_t bitmap[BITMAP_BLOCKS * LEANFS_BLOCK_SIZE];

_Static_assert((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES) % LEANFS_BLOCK_SIZE == 0,
               "the inode table must be a whole number of blocks so save_meta can write it in place");

static void block_read(uint32_t block, void *destination);
static void block_write(uint32_t block, const void *source);
static void block_write_meta(uint32_t block, const void *source);

/* M194: with the journal attached, what reaches the disk is a transaction
   the block layer commits whole, so the barrier after every metadata block
   that M71 and M104 ordered the disk with is not needed - and on a USB stick
   it was most of what a browser spent starting. Without one (a device too
   small for it, or the host tests' fake disk) the barriers are exactly as
   they were. */
static int journal_active;

/* M198: a number that changes whenever an inode's contents might have - a
   write, a truncation, the inode being freed - and never takes a value it has
   had before, even for an inode number reused by another file or a disk
   mounted again. The program image cache keys on it, so a binary rewritten on
   disk is read again. It lives only in memory, and it lives beside the inode
   table rather than in the kernel image: half a megabyte of bss pushed the
   kernel's end past a range the laptop's firmware keeps, and the boot loader
   could not place it at all. */
static uint32_t *inode_generation;
static uint32_t generation_clock;

static void inodes_alloc(void) {
    uint64_t frames = (sizeof(leanfs_inode_t) * (uint64_t)LEANFS_MAX_INODES) / 4096;
    if (!inodes) {
        uint64_t base = physical_memory_alloc_contiguous(frames);
        inodes = (leanfs_inode_t *)(uintptr_t)base;
    }
    k_memset(inodes, 0, sizeof(leanfs_inode_t) * (size_t)LEANFS_MAX_INODES);
    if (!inode_generation) {
        uint64_t generation_frames = (sizeof(uint32_t) * (uint64_t)LEANFS_MAX_INODES + 4095) / 4096;
        inode_generation = (uint32_t *)(uintptr_t)physical_memory_alloc_contiguous(generation_frames);
    }
    uint32_t epoch = ++generation_clock;
    for (uint32_t i = 0; i < LEANFS_MAX_INODES; i++) {
        inode_generation[i] = epoch;
    }
}

static uint8_t directory_block[LEANFS_BLOCK_SIZE];

static int directory_hint_inode = -1;
static uint32_t directory_hint_block = 0;

static void save_superblock(void) {
    uint8_t buffer[LEANFS_BLOCK_SIZE];
    k_memset(buffer, 0, sizeof(buffer));
    k_memcpy(buffer, &sb, sizeof(sb));
    block_write_meta(LEANFS_START_BLOCK, buffer);
}

static uint8_t inode_block_dirty[INODE_TABLE_BLOCKS];
static uint8_t bitmap_block_dirty[BITMAP_BLOCKS];

static uint32_t meta_writes;

static void mark_inode(int idx) {
    size_t byte = (size_t)idx * sizeof(leanfs_inode_t);
    size_t first = byte / LEANFS_BLOCK_SIZE;
    size_t last = (byte + sizeof(leanfs_inode_t) - 1) / LEANFS_BLOCK_SIZE;
    for (size_t sec = first; sec <= last && sec < INODE_TABLE_BLOCKS; sec++) {
        inode_block_dirty[sec] = 1;
    }
}

static void mark_all_inodes(void) {
    for (size_t i = 0; i < INODE_TABLE_BLOCKS; i++) {
        inode_block_dirty[i] = 1;
    }
}

static void mark_block_bit(uint32_t bit) {
    size_t sec = (bit / 8u) / LEANFS_BLOCK_SIZE;
    if (sec < BITMAP_BLOCKS) {
        bitmap_block_dirty[sec] = 1;
    }
}

static void mark_all_blocks(void) {
    for (size_t i = 0; i < BITMAP_BLOCKS; i++) {
        bitmap_block_dirty[i] = 1;
    }
}

static int io_error;

/* M176: the two indirect tables the last map_block read, remembered so a
   sequential read does not fetch them again for every block. A 213 MiB
   program is 54,528 blocks, and every one of them past the direct range used
   to cost two extra 4 KiB reads - the double-indirect root, which is the SAME
   block for the whole file, and an inner table that changes once every 1024
   blocks. Two thirds of the device traffic in a large read was re-reading
   those two.

   Correctness comes from throwing it away rather than from tracking writes:
   every leanfs operation starts with io_begin, and any call that may ALLOCATE
   drops it too, so a cached table cannot outlive a change to one. */
static uint32_t table_cache_root_block;
static uint32_t table_cache_root[LEANFS_INDIRECT_POINTERS];
static int table_cache_root_valid;
static uint32_t table_cache_leaf_block;
static uint32_t table_cache_leaf[LEANFS_INDIRECT_POINTERS];
static int table_cache_leaf_valid;

static void table_cache_forget(void) {
    table_cache_root_valid = 0;
    table_cache_leaf_valid = 0;
}

static void io_begin(void) {
    io_error = 0;
    table_cache_forget();
}

static int io_failed(void) {
    return io_error;
}

/* M176: how many neighbouring blocks one device request may carry. A MiB is
   what tests/budgets.tsv measures the disk at, and the block cache's hit path
   copies a sector at a time, so a larger run would trade device calls for
   memcpy calls rather than remove work. */
#define LEANFS_READ_RUN_BLOCKS 256

static void block_read_run(uint32_t block, uint32_t count, void *destination) {
    if (block_device_read(block * LEANFS_SECTORS_PER_BLOCK,
                          count * LEANFS_SECTORS_PER_BLOCK, destination) != 0) {
        io_error = 1;
        k_memset(destination, 0, (size_t)count * LEANFS_BLOCK_SIZE);
    }
}

static void block_read(uint32_t block, void *destination) {
    if (block_device_read(block * LEANFS_SECTORS_PER_BLOCK, LEANFS_SECTORS_PER_BLOCK, destination) != 0) {
        io_error = 1;
    }
}

static void block_write(uint32_t block, const void *source) {
    if (block_device_write(block * LEANFS_SECTORS_PER_BLOCK, LEANFS_SECTORS_PER_BLOCK, source) != 0) {
        io_error = 1;
    }
}

static void block_write_meta(uint32_t block, const void *source) {
    /* M176: every indirect table is written through here, so this is where a
       cached copy of one stops being true. io_begin and the allocating path
       already drop it, but an operation that frees blocks and then reads the
       map again would sit between the two - and a cache that is right except
       in one path is not a cache, it is a bug with a schedule. */
    table_cache_forget();
    if (block_device_write(block * LEANFS_SECTORS_PER_BLOCK, LEANFS_SECTORS_PER_BLOCK, source) != 0) {
        io_error = 1;
    }
    if (!journal_active && block_device_flush() != 0) {
        io_error = 1;
    }
}

static void write_run(uint32_t block, size_t blocks, const uint8_t *source) {
    if (block_device_write(block * LEANFS_SECTORS_PER_BLOCK,
                  (uint32_t)blocks * LEANFS_SECTORS_PER_BLOCK, source) != 0) {
        io_error = 1;
    }
}

static void read_run(uint32_t block, size_t blocks, uint8_t *destination) {
    if (block_device_read(block * LEANFS_SECTORS_PER_BLOCK,
                 (uint32_t)blocks * LEANFS_SECTORS_PER_BLOCK, destination) != 0) {
        io_error = 1;
    }
}

static void save_meta(void) {
    if (!journal_active) {
        block_device_flush();
    }

    const uint8_t *inode_bytes = (const uint8_t *)inodes;

    for (size_t i = 0; i < INODE_TABLE_BLOCKS; ) {
        if (!inode_block_dirty[i]) {
            i++;
            continue;
        }
        size_t run = 0;
        while (i + run < INODE_TABLE_BLOCKS && inode_block_dirty[i + run]) {
            inode_block_dirty[i + run] = 0;
            run++;
        }
        write_run(sb.inode_table_block + (uint32_t)i, run,
                  inode_bytes + i * LEANFS_BLOCK_SIZE);
        meta_writes += (uint32_t)run;
        i += run;
    }

    for (size_t i = 0; i < BITMAP_BLOCKS; ) {
        if (!bitmap_block_dirty[i]) {
            i++;
            continue;
        }
        size_t run = 0;
        while (i + run < BITMAP_BLOCKS && bitmap_block_dirty[i + run]) {
            bitmap_block_dirty[i + run] = 0;
            run++;
        }
        write_run(sb.bitmap_block + (uint32_t)i, run,
                  bitmap + i * LEANFS_BLOCK_SIZE);
        meta_writes += (uint32_t)run;
        i += run;
    }
    if (!journal_active) {
        block_device_flush();
    }
}

uint32_t leanfs_meta_writes(void) {
    return meta_writes;
}

static void format(void) {
    kernel_log_puts("[fs] no valid leanfs superblock found - formatting fresh\n");

    sb.magic = LEANFS_MAGIC;
    sb.inode_table_block = LEANFS_START_BLOCK + 1;
    sb.inode_table_blocks = INODE_TABLE_BLOCKS;
    sb.bitmap_block = sb.inode_table_block + INODE_TABLE_BLOCKS;
    sb.bitmap_blocks_field = BITMAP_BLOCKS;
    sb.data_block = sb.bitmap_block + BITMAP_BLOCKS;
    sb.data_blocks = LEANFS_DATA_BLOCKS;
    sb.state = LEANFS_STATE_CLEAN;
    sb.version = LEANFS_VERSION;

    k_memset(inodes, 0, sizeof(inodes));
    k_memset(bitmap, 0, sizeof(bitmap));
    bitmap[0] |= 1u;
    mark_block_bit(0);

    inodes[ROOT_INODE].type = LEANFS_TYPE_DIRECTORY;
    inodes[ROOT_INODE].size = 0;
    inodes[ROOT_INODE].nlink = 1;

    save_superblock();
    mark_all_inodes();
    mark_all_blocks();
    save_meta();
    if (journal_active) {
        block_device_journal_reset();
        block_device_checkpoint();
    }
}

static void format_if_this_disk_is_ours(void) {
    uint8_t first_block[LEANFS_BLOCK_SIZE];
    block_read(0, first_block);
    if (!leanfs_disk_carries_this_os(first_block)) {
        kernel_log_puts("[fs] the disk this kernel is reading does not carry lean_os's own boot "
                   "sector. It belongs to some other system, and formatting it would destroy "
                   "whatever is on it. Nothing has been written.\n");
        panic("refusing to format a disk that is not this OS's");
    }
    format();
}

void leanfs_init(void) {
    leanfs_forget_holds();
    inodes_alloc();
    /* Replay before anything is read: the superblock itself may be in a
       committed transaction that never reached home. */
    int replayed = block_device_journal_attach(LEANFS_START_LBA,
                                               LEANFS_TOTAL_BLOCKS * LEANFS_SECTORS_PER_BLOCK,
                                               LEANFS_JOURNAL_START_LBA, LEANFS_JOURNAL_BLOCKS);
    journal_active = replayed >= 0;
    if (replayed > 0) {
        kernel_log_puts("[fs] journal: ");
        kernel_log_put_dec((uint32_t)replayed);
        kernel_log_puts(" committed transaction(s) replayed\n");
    }
    uint8_t buffer[LEANFS_BLOCK_SIZE];
    block_read(LEANFS_START_BLOCK, buffer);
    k_memcpy(&sb, buffer, sizeof(sb));

    if (sb.magic != LEANFS_MAGIC ||
        sb.data_blocks != LEANFS_DATA_BLOCKS ||
        sb.inode_table_blocks != INODE_TABLE_BLOCKS ||
        sb.bitmap_blocks_field != BITMAP_BLOCKS) {
        format_if_this_disk_is_ours();
    } else if (sb.version != LEANFS_VERSION) {
        kernel_log_puts("[fs] leanfs on-disk version is not this build's - reformatting\n");
        format_if_this_disk_is_ours();
    } else {
        read_run(sb.inode_table_block, sb.inode_table_blocks, (uint8_t *)inodes);
        read_run(sb.bitmap_block, sb.bitmap_blocks_field, bitmap);

        if (inodes[ROOT_INODE].type != LEANFS_TYPE_DIRECTORY) {
            kernel_log_puts("[fs] leanfs root inode is not a directory - reformatting\n");
            format_if_this_disk_is_ours();
        } else if (sb.state == LEANFS_STATE_DIRTY) {
            kernel_log_puts("[fs] leanfs was not cleanly unmounted - checking\n");
            leanfs_check();
        }
    }

    sb.state = LEANFS_STATE_DIRTY;
    save_superblock();

    kernel_log_puts("[fs] leanfs ready: data_block=0x");
    kernel_log_put_hex32(sb.data_block);
    kernel_log_puts(" data_blocks=0x");
    kernel_log_put_hex32(sb.data_blocks);
    kernel_log_puts(" inodes=0x");
    kernel_log_put_hex32(LEANFS_MAX_INODES);
    kernel_log_putc('\n');
}

static int bitmap_test(uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
    mark_block_bit(bit);
}

static void bitmap_clear(uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    mark_block_bit(bit);
}

static int block_valid(uint32_t block) {
    return block < sb.data_blocks;
}

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
    return idx >= 0 && idx < (int)LEANFS_MAX_INODES && inodes[idx].type != LEANFS_TYPE_FREE;
}

static void inode_changed(int idx) {
    if (idx >= 0 && idx < (int)LEANFS_MAX_INODES) {
        inode_generation[idx] = ++generation_clock;
    }
}

static int find_free_inode(void) {
    for (int i = 0; i < (int)LEANFS_MAX_INODES; i++) {
        if (inodes[i].type == LEANFS_TYPE_FREE) {
            return i;
        }
    }
    return -1;
}

static int64_t map_block(int idx, uint32_t logical, int allocate) {
    leanfs_inode_t *inode = &inodes[idx];
    static uint32_t table[LEANFS_INDIRECT_POINTERS];

    if (allocate) {
        table_cache_forget();
    }

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
        int block_device = alloc_block();
        if (block_device < 0) {
            return -1;
        }
        inode->direct[logical] = (uint32_t)block_device;
        mark_inode(idx);
        return block_device;
    }

    uint32_t rel = logical - LEANFS_DIRECT_BLOCKS;
    uint32_t table_block;
    uint32_t slot;
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
        block_write_meta(sb.data_block + (uint32_t)t, (const uint8_t *)table);
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
        uint32_t outer_entry;
        if (!allocate && table_cache_root_valid && table_cache_root_block == root) {
            /* Read the one slot wanted rather than copying the table to get
               at it. Copying 4 KiB to look up 4 bytes is what the first
               version of this cache did, and it spent more on the memcpy
               than the block cache had been spending on the lookup. */
            outer_entry = table_cache_root[outer];
        } else {
            block_read(sb.data_block + root, (uint8_t *)table);
            if (!allocate) {
                k_memcpy(table_cache_root, table, sizeof(table_cache_root));
                table_cache_root_block = root;
                table_cache_root_valid = 1;
            }
            outer_entry = table[outer];
        }
        if (!block_present(outer_entry)) {
            if (!allocate) {
                return -1;
            }
            int t = alloc_block();
            if (t < 0) {
                return -1;
            }
            table[outer] = (uint32_t)t;
            block_write_meta(sb.data_block + root, (const uint8_t *)table);
            static uint32_t empty[LEANFS_INDIRECT_POINTERS];
            k_memset(empty, 0, sizeof(empty));
            block_write_meta(sb.data_block + (uint32_t)t, (const uint8_t *)empty);
            outer_entry = (uint32_t)t;
        }
        table_block = outer_entry;
    }

    if (!allocate && table_cache_leaf_valid && table_cache_leaf_block == table_block) {
        uint32_t entry = table_cache_leaf[slot];
        return block_present(entry) ? (int64_t)entry : -1;
    }
    block_read(sb.data_block + table_block, (uint8_t *)table);
    if (!allocate) {
        k_memcpy(table_cache_leaf, table, sizeof(table_cache_leaf));
        table_cache_leaf_block = table_block;
        table_cache_leaf_valid = 1;
    }
    if (block_present(table[slot])) {
        return table[slot];
    }
    if (!allocate) {
        return -1;
    }
    int block_device = alloc_block();
    if (block_device < 0) {
        return -1;
    }
    table[slot] = (uint32_t)block_device;
    block_write_meta(sb.data_block + table_block, (const uint8_t *)table);
    return block_device;
}

static uint8_t check_bitmap[BITMAP_BLOCKS * LEANFS_BLOCK_SIZE];

static void check_mark(uint32_t block) {
    if (block_valid(block)) {
        check_bitmap[block / 8] |= (uint8_t)(1u << (block % 8));
    }
}

uint32_t leanfs_check(void) {
    k_memset(check_bitmap, 0, sizeof(check_bitmap));
    check_mark(0);

    static uint32_t table[LEANFS_INDIRECT_POINTERS];
    for (int idx = 0; idx < (int)LEANFS_MAX_INODES; idx++) {
        leanfs_inode_t *inode = &inodes[idx];
        if (inode->type == LEANFS_TYPE_FREE) {
            continue;
        }
        uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
        for (uint32_t b = 0; b < nblocks; b++) {
            int64_t block_device = map_block(idx, b, 0);
            if (block_device >= 0) {
                check_mark((uint32_t)block_device);
            }
        }
        if (block_present(inode->indirect)) {
            check_mark(inode->indirect);
        }
        if (block_present(inode->dindirect)) {
            check_mark(inode->dindirect);
            block_read(sb.data_block + inode->dindirect, (uint8_t *)table);
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

    kernel_log_puts("[fs] leanfs check: ");
    kernel_log_put_dec(orphans);
    kernel_log_puts(" orphaned block(s) reclaimed, ");
    kernel_log_put_dec(missing);
    kernel_log_puts(" block(s) were in use but marked free");
    kernel_log_puts(orphans || missing ? " - bitmap rebuilt from the inodes\n" : " - nothing to fix\n");

    return orphans + missing;
}

static void free_inode_blocks(int idx) {
    inode_changed(idx);
    leanfs_inode_t *inode = &inodes[idx];
    uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
    static uint32_t table[LEANFS_INDIRECT_POINTERS];

    for (uint32_t b = 0; b < nblocks; b++) {
        int64_t block_device = map_block(idx, b, 0);
        if (block_device >= 0) {
            bitmap_clear((uint32_t)block_device);
        }
    }
    if (block_present(inode->indirect)) {
        bitmap_clear(inode->indirect);
    }
    if (block_present(inode->dindirect)) {
        block_read(sb.data_block + inode->dindirect, (uint8_t *)table);
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

static int64_t inode_pread(int idx, void *buffer, size_t length, uint32_t off) {
    leanfs_inode_t *inode = &inodes[idx];
    if (off >= inode->size) {
        return 0;
    }
    size_t avail = inode->size - off;
    if (length > avail) {
        length = avail;
    }
    static uint8_t block_buffer[LEANFS_BLOCK_SIZE];
    size_t copied = 0;
    while (copied < length) {
        uint32_t position = off + (uint32_t)copied;
        int64_t block_device = map_block(idx, position / LEANFS_BLOCK_SIZE, 0);
        size_t within = position % LEANFS_BLOCK_SIZE;
        size_t chunk = LEANFS_BLOCK_SIZE - within;
        if (chunk > length - copied) {
            chunk = length - copied;
        }
        if (block_device < 0) {
            k_memset((uint8_t *)buffer + copied, 0, chunk);
        } else if (within == 0 && chunk == LEANFS_BLOCK_SIZE) {
            /* M176: a whole block going to a whole block needs no staging.
               Every byte of a large read used to be copied twice - once from
               the device into block_buffer and once out of it - for the sake
               of the partial block at each end, which is what `within` and
               `chunk` are already measuring.

               And blocks that are next to each other on the disk are asked
               for together. The block cache's line is one leanfs block, so
               one call per block is one DEVICE read per block, and a 4 KiB
               request costs a device about what a much larger one does -
               which is why the block layer's own budget measures a MiB at a
               time and reaches four times this rate. The run is capped so the
               cache's per-sector loop on a hit cannot grow without bound. */
            uint32_t run = 1;
            while (run < LEANFS_READ_RUN_BLOCKS &&
                   copied + (size_t)(run + 1) * LEANFS_BLOCK_SIZE <= length) {
                int64_t next = map_block(idx, position / LEANFS_BLOCK_SIZE + run, 0);
                if (next != block_device + run) {
                    break;
                }
                run++;
            }
            block_read_run(sb.data_block + (uint32_t)block_device, run,
                           (uint8_t *)buffer + copied);
            copied += (size_t)run * LEANFS_BLOCK_SIZE;
            continue;
        } else {
            block_read(sb.data_block + (uint32_t)block_device, block_buffer);
            k_memcpy((uint8_t *)buffer + copied, block_buffer + within, chunk);
        }
        copied += chunk;
    }
    return (int64_t)copied;
}

static int64_t inode_pwrite(int idx, const void *buffer, size_t length, uint32_t off) {
    inode_changed(idx);
    leanfs_inode_t *inode = &inodes[idx];
    if (off > (uint32_t)LEANFS_MAX_FILE_SIZE || length > (size_t)LEANFS_MAX_FILE_SIZE - off) {
        return -1;
    }
    static uint8_t block_buffer[LEANFS_BLOCK_SIZE];
    size_t written = 0;
    while (written < length) {
        uint32_t position = off + (uint32_t)written;
        uint32_t logical = position / LEANFS_BLOCK_SIZE;
        size_t within = position % LEANFS_BLOCK_SIZE;
        size_t chunk = LEANFS_BLOCK_SIZE - within;
        if (chunk > length - written) {
            chunk = length - written;
        }
        int64_t block_device = map_block(idx, logical, 1);
        if (block_device < 0) {
            break;
        }
        if (chunk != LEANFS_BLOCK_SIZE) {
            uint32_t block_start = logical * (uint32_t)LEANFS_BLOCK_SIZE;
            if (block_start < inode->size) {
                block_read(sb.data_block + (uint32_t)block_device, block_buffer);
            } else {
                k_memset(block_buffer, 0, sizeof(block_buffer));
            }
        }
        k_memcpy(block_buffer + within, (const uint8_t *)buffer + written, chunk);
        block_write(sb.data_block + (uint32_t)block_device,
                           chunk == LEANFS_BLOCK_SIZE ? (const uint8_t *)buffer + written : block_buffer);
        written += chunk;
        if (position + chunk > inode->size) {
            inode->size = position + (uint32_t)chunk;
            mark_inode(idx);
        }
    }
    if (written > 0) {
        inode->mtime = rtc_now();
        mark_inode(idx);
    }
    return written == 0 && length > 0 ? -1 : (int64_t)written;
}

static int64_t inode_read_data(int idx, void *buffer, size_t maxlen) {
    int64_t n = inode_pread(idx, buffer, maxlen, 0);
    if (n < 0) {
        return -1;
    }
    return (int64_t)inodes[idx].size;
}

static int inode_write_data(int idx, const void *buffer, size_t length) {
    if (length > (size_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    free_inode_blocks(idx);
    inodes[idx].size = 0;
    mark_inode(idx);
    if (length == 0) {
        inodes[idx].mtime = rtc_now();
        return 0;
    }
    if (inode_pwrite(idx, buffer, length, 0) != (int64_t)length) {
        free_inode_blocks(idx);
        inodes[idx].size = 0;
        mark_inode(idx);
        return -1;
    }
    return 0;
}

static leanfs_dirent_t *directory_rec(uint32_t off) {
    return (leanfs_dirent_t *)(directory_block + off);
}

static int directory_block_valid(void) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        if (off + LEANFS_DIRENT_HEADER > LEANFS_BLOCK_SIZE) {
            return 0;
        }
        leanfs_dirent_t *r = directory_rec(off);
        if (r->rec_length < LEANFS_DIRENT_HEADER ||
            (r->rec_length % LEANFS_DIRENT_ALIGN) != 0 ||
            off + r->rec_length > LEANFS_BLOCK_SIZE ||
            LEANFS_DIRENT_NEED(r->name_length) > r->rec_length) {
            return 0;
        }
        off += r->rec_length;
    }
    return off == LEANFS_BLOCK_SIZE;
}

static void directory_block_init(void) {
    k_memset(directory_block, 0, LEANFS_BLOCK_SIZE);
    directory_rec(0)->rec_length = (uint16_t)LEANFS_BLOCK_SIZE;
}

static int directory_block_read(int idx, uint32_t logical) {
    int64_t block_device = map_block(idx, logical, 0);
    if (block_device < 0) {
        directory_block_init();
        return 0;
    }
    block_read(sb.data_block + (uint32_t)block_device, directory_block);
    if (!directory_block_valid()) {
        return -1;
    }
    return 1;
}

static int directory_block_write(int idx, uint32_t logical) {
    int64_t block_device = map_block(idx, logical, 1);
    if (block_device < 0) {
        return -1;
    }
    block_write_meta(sb.data_block + (uint32_t)block_device, directory_block);
    inodes[idx].mtime = rtc_now();
    mark_inode(idx);
    return 0;
}

static uint32_t directory_nblocks(int idx) {
    return inodes[idx].size / LEANFS_BLOCK_SIZE;
}

static int directory_ok(int idx) {
    return inode_valid(idx) && inodes[idx].type == LEANFS_TYPE_DIRECTORY;
}

static int32_t directory_block_find(const char *name, uint32_t name_length) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        leanfs_dirent_t *r = directory_rec(off);
        if (r->inode != 0 && r->name_length == name_length &&
            k_memcmp(directory_block + off + LEANFS_DIRENT_HEADER, name, name_length) == 0) {
            return (int32_t)off;
        }
        off += r->rec_length;
    }
    return -1;
}

static void directory_block_coalesce(void) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        leanfs_dirent_t *r = directory_rec(off);
        if (r->inode == 0) {
            uint32_t next = off + r->rec_length;
            while (next < LEANFS_BLOCK_SIZE && directory_rec(next)->inode == 0) {
                r->rec_length = (uint16_t)(r->rec_length + directory_rec(next)->rec_length);
                next = off + r->rec_length;
            }
            r->name_length = 0;
            r->type = 0;
        }
        off += directory_rec(off)->rec_length;
    }
}

static int directory_block_place(const char *name, uint32_t name_length, int inode_index) {
    uint32_t need = LEANFS_DIRENT_NEED(name_length);

    for (int pass = 0; pass < 2; pass++) {
        uint32_t off = 0;
        while (off < LEANFS_BLOCK_SIZE) {
            leanfs_dirent_t *r = directory_rec(off);
            uint32_t place_at = 0;
            uint32_t place_length = 0;

            if (pass == 0 && r->inode == 0 && r->rec_length >= need) {
                place_at = off;
                place_length = r->rec_length;
            } else if (pass == 1 && r->inode != 0) {
                uint32_t used = LEANFS_DIRENT_NEED(r->name_length);
                if (r->rec_length >= used + need) {
                    place_length = r->rec_length - used;
                    r->rec_length = (uint16_t)used;
                    place_at = off + used;
                }
            }

            if (place_length > 0) {
                leanfs_dirent_t *n = directory_rec(place_at);
                n->inode = (uint32_t)inode_index;
                n->rec_length = (uint16_t)place_length;
                n->name_length = (uint8_t)name_length;
                n->type = (uint8_t)inodes[inode_index].type;
                k_memcpy(directory_block + place_at + LEANFS_DIRENT_HEADER, name, name_length);
                return 1;
            }
            off += r->rec_length;
        }
    }
    return 0;
}

static int directory_lookup(int directory, const char *name) {
    if (!directory_ok(directory)) {
        return -1;
    }
    uint32_t name_length = (uint32_t)k_strlen(name);
    uint32_t blocks = directory_nblocks(directory);
    for (uint32_t b = 0; b < blocks; b++) {
        if (directory_block_read(directory, b) < 0) {
            return -1;
        }
        int32_t off = directory_block_find(name, name_length);
        if (off >= 0) {
            return (int)directory_rec((uint32_t)off)->inode;
        }
    }
    return -1;
}

static int directory_add(int directory, const char *name, int inode_index) {
    if (!directory_ok(directory) || !inode_valid(inode_index)) {
        return -1;
    }
    uint32_t name_length = (uint32_t)k_strlen(name);
    if (name_length == 0 || name_length > LEANFS_MAX_NAME) {
        return -1;
    }

    uint32_t blocks = directory_nblocks(directory);
    uint32_t start = (directory == directory_hint_inode && directory_hint_block < blocks) ? directory_hint_block : 0;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t from = (pass == 0) ? start : 0;
        uint32_t to = (pass == 0) ? blocks : start;
        for (uint32_t b = from; b < to; b++) {
            if (directory_block_read(directory, b) < 0) {
                return -1;
            }
            if (directory_block_place(name, name_length, inode_index)) {
                directory_hint_inode = directory;
                directory_hint_block = b;
                return directory_block_write(directory, b);
            }
        }
        if (start == 0) {
            break;
        }
    }

    if (inodes[directory].size > (uint32_t)LEANFS_MAX_FILE_SIZE - LEANFS_BLOCK_SIZE) {
        return -1;
    }
    directory_block_init();
    if (!directory_block_place(name, name_length, inode_index)) {
        return -1;
    }
    if (directory_block_write(directory, blocks) != 0) {
        return -1;
    }
    directory_hint_inode = directory;
    directory_hint_block = blocks;
    inodes[directory].size += LEANFS_BLOCK_SIZE;
    mark_inode(directory);
    return 0;
}

static int directory_repoint(int directory, const char *name, int inode_index) {
    if (!directory_ok(directory) || !inode_valid(inode_index)) {
        return -1;
    }
    uint32_t name_length = (uint32_t)k_strlen(name);
    uint32_t blocks = directory_nblocks(directory);
    for (uint32_t b = 0; b < blocks; b++) {
        if (directory_block_read(directory, b) < 0) {
            return -1;
        }
        int32_t off = directory_block_find(name, name_length);
        if (off >= 0) {
            leanfs_dirent_t *r = directory_rec((uint32_t)off);
            r->inode = (uint32_t)inode_index;
            r->type = (uint8_t)inodes[inode_index].type;
            return directory_block_write(directory, b);
        }
    }
    return -1;
}

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
        return -1;
    }
    if (out[0] == '.' && (out[1] == '\0' || (out[1] == '.' && out[2] == '\0'))) {
        return -1;
    }
    if (*s == '/') {
        s++;
    }
    *p = s;
    return 1;
}

#define LEANFS_MAX_LINK_HOPS 8

static char walk_path[LEANFS_MAX_PATH];
static char walk_next[LEANFS_MAX_PATH];

static int read_link_target(int idx, char *out, size_t cap) {
    uint32_t n = inodes[idx].size;
    if (n == 0 || n >= cap) {
        return -1;
    }
    if (inode_pread(idx, out, n, 0) != (int64_t)n) {
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

static int resolve_ex(const char *path, int follow_final) {
    if (!path || path[0] != '/') {
        return -1;
    }
    k_strlcpy(walk_path, path, sizeof(walk_path));

    for (int hop = 0; hop <= LEANFS_MAX_LINK_HOPS; hop++) {
        if (walk_path[1] == '\0') {
            return ROOT_INODE;
        }
        const char *p = walk_path + 1;
        int at = ROOT_INODE;
        char comp[LEANFS_MAX_NAME + 1];
        int rc;
        int rewritten = 0;

        while ((rc = next_component(&p, comp)) == 1) {
            if (inodes[at].type != LEANFS_TYPE_DIRECTORY) {
                return -1;
            }
            at = directory_lookup(at, comp);
            if (!inode_valid(at)) {
                return -1;
            }
            if (inodes[at].type != LEANFS_TYPE_LINK) {
                continue;
            }
            int is_final = (*p == '\0');
            if (is_final && !follow_final) {
                break;
            }

            char target[LEANFS_MAX_PATH];
            if (read_link_target(at, target, sizeof(target)) < 0) {
                return -1;
            }

            uint32_t n = 0;
            if (target[0] != '/') {
                size_t comp_length = k_strlen(comp);
                const char *after = p;
                size_t prefix_length = (size_t)(after - walk_path);
                prefix_length -= comp_length;
                while (prefix_length > 1 && walk_path[prefix_length - 1] == '/') {
                    prefix_length--;
                }
                if (prefix_length >= sizeof(walk_next) - 2) {
                    return -1;
                }
                k_memcpy(walk_next, walk_path, prefix_length);
                n = (uint32_t)prefix_length;
                if (n == 0 || walk_next[n - 1] != '/') {
                    walk_next[n++] = '/';
                }
            }
            size_t tlen = k_strlen(target);
            if (n + tlen + 1 >= sizeof(walk_next)) {
                return -1;
            }
            k_memcpy(walk_next + n, target, tlen);
            n += (uint32_t)tlen;
            if (*p != '\0') {
                if (walk_next[n - 1] != '/') {
                    walk_next[n++] = '/';
                }
                size_t rest = k_strlen(p);
                if (n + rest + 1 >= sizeof(walk_next)) {
                    return -1;
                }
                k_memcpy(walk_next + n, p, rest);
                n += (uint32_t)rest;
            }
            if (*p == '\0') {
                size_t was = k_strlen(walk_path);
                if (was > 1 && walk_path[was - 1] == '/' &&
                    n + 1 < sizeof(walk_next) && walk_next[n - 1] != '/') {
                    walk_next[n++] = '/';
                }
            }
            walk_next[n] = '\0';
            k_strlcpy(walk_path, walk_next, sizeof(walk_path));
            rewritten = 1;
            break;
        }

        if (rewritten) {
            continue;
        }
        if (rc < 0) {
            return -1;
        }
        size_t length = k_strlen(walk_path);
        if (length > 1 && walk_path[length - 1] == '/' && inodes[at].type != LEANFS_TYPE_DIRECTORY) {
            return -1;
        }
        return at;
    }
    return -1;
}

static int resolve(const char *path) {
    return resolve_ex(path, 1);
}

static int resolve_nofollow(const char *path) {
    return resolve_ex(path, 0);
}

static int resolve_parent(const char *path, int *out_parent, char *out_leaf) {
    if (!path || path[0] != '/' || path[1] == '\0') {
        return -1;
    }
    const char *last = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/') {
            last = s;
        }
    }
    if (last[1] == '\0') {
        return -1;
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
        parent = ROOT_INODE;
    } else {
        char directory_path[LEANFS_MAX_PATH];
        size_t directory_length = (size_t)(last - path);
        if (directory_length >= sizeof(directory_path)) {
            return -1;
        }
        k_memcpy(directory_path, path, directory_length);
        directory_path[directory_length] = '\0';
        parent = resolve(directory_path);
    }
    if (!inode_valid(parent) || inodes[parent].type != LEANFS_TYPE_DIRECTORY) {
        return -1;
    }
    *out_parent = parent;
    return 0;
}

static int directory_remove(int directory, const char *name) {
    if (!directory_ok(directory)) {
        return -1;
    }
    uint32_t name_length = (uint32_t)k_strlen(name);
    uint32_t blocks = directory_nblocks(directory);
    for (uint32_t b = 0; b < blocks; b++) {
        if (directory_block_read(directory, b) < 0) {
            return -1;
        }
        int32_t off = directory_block_find(name, name_length);
        if (off >= 0) {
            leanfs_dirent_t *r = directory_rec((uint32_t)off);
            int idx = (int)r->inode;
            r->inode = 0;
            r->name_length = 0;
            r->type = 0;
            directory_block_coalesce();
            if (directory != directory_hint_inode || b < directory_hint_block) {
                directory_hint_inode = directory;
                directory_hint_block = b;
            }
            return directory_block_write(directory, b) == 0 ? idx : -1;
        }
    }
    return -1;
}

static int directory_is_empty(int idx) {
    if (!directory_ok(idx)) {
        return 0;
    }
    uint32_t blocks = directory_nblocks(idx);
    for (uint32_t b = 0; b < blocks; b++) {
        if (directory_block_read(idx, b) < 0) {
            return 0;
        }
        uint32_t off = 0;
        while (off < LEANFS_BLOCK_SIZE) {
            if (directory_rec(off)->inode != 0) {
                return 0;
            }
            off += directory_rec(off)->rec_length;
        }
    }
    return 1;
}

int leanfs_exists(const char *path) {
    return inode_valid(resolve(path));
}

uint32_t leanfs_free_scratch_lba(uint32_t blocks) {
    /* With a journal, free data blocks are no longer free to scribble on
       behind the filesystem's back - the write would join a transaction. The
       journal's own area is, just after a checkpoint has emptied it. */
    if (journal_active) {
        return blocks == 0 ? 0 : block_device_journal_scratch_lba(blocks);
    }
    if (blocks == 0 || blocks >= sb.data_blocks) {
        return 0;
    }
    uint32_t first = sb.data_blocks - blocks;
    for (uint32_t b = first; b < sb.data_blocks; b++) {
        if (bitmap_test(b)) {
            return 0;
        }
    }
    return (sb.data_block + first) * LEANFS_SECTORS_PER_BLOCK;
}

int leanfs_is_directory(const char *path) {
    int idx = resolve(path);
    return inode_valid(idx) && inodes[idx].type == LEANFS_TYPE_DIRECTORY;
}

int64_t leanfs_read(const char *path, void *buffer, size_t maxlen) {
    io_begin();
    int idx = resolve(path);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    int64_t n = inode_read_data(idx, buffer, maxlen);
    return io_failed() ? -1 : n;
}

int leanfs_write(const char *path, const void *buffer, size_t length) {
    if (length > LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    io_begin();
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }

    int idx = directory_lookup(parent, leaf);
    if (inode_valid(idx)) {
        if (inodes[idx].type != LEANFS_TYPE_FILE) {
            return -1;
        }
    } else {
        idx = find_free_inode();
        if (idx < 0) {
            return -1;
        }
        k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
        inodes[idx].type = LEANFS_TYPE_FILE;
        inodes[idx].nlink = 1;
        mark_inode(idx);
        if (directory_add(parent, leaf, idx) != 0) {
            inodes[idx].type = LEANFS_TYPE_FREE;
            return -1;
        }
    }

    if (inode_write_data(idx, buffer, length) != 0) {
        return -1;
    }
    save_meta();
    return io_failed() ? -1 : 0;
}

int leanfs_mkdir(const char *path) {
    io_begin();
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    if (inode_valid(directory_lookup(parent, leaf))) {
        return -1;
    }
    int idx = find_free_inode();
    if (idx < 0) {
        return -1;
    }
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    inodes[idx].type = LEANFS_TYPE_DIRECTORY;
    inodes[idx].size = 0;
    inodes[idx].nlink = 1;
    mark_inode(idx);
    if (directory_add(parent, leaf, idx) != 0) {
        inodes[idx].type = LEANFS_TYPE_FREE;
        mark_inode(idx);
        return -1;
    }
    save_meta();
    return io_failed() ? -1 : 0;
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

uint32_t leanfs_total_blocks(void) {
    return sb.data_blocks;
}

uint32_t leanfs_total_inodes(void) {
    return (uint32_t)LEANFS_MAX_INODES;
}

uint32_t leanfs_free_inodes(void) {
    uint32_t free_count = 0;
    for (int i = 0; i < (int)LEANFS_MAX_INODES; i++) {
        if (inodes[i].type == LEANFS_TYPE_FREE) {
            free_count++;
        }
    }
    return free_count;
}

int leanfs_utime(const char *path, uint32_t mtime) {
    int idx = resolve(path);
    if (idx < 0) {
        return -1;
    }
    inodes[idx].mtime = mtime;
    mark_inode(idx);
    save_meta();
    return 0;
}

/* Orphans. A file that is unlinked - or replaced by a rename - while it is
   still open loses its NAME at once and its inode and blocks only when the
   last holder lets go, which is what POSIX says and what every program that
   writes a temporary file and deletes it while holding it relies on.

   This filesystem freed the inode on the spot. Chromium maps its persistent
   histogram file MAP_SHARED, shares it with its children and unlinks it while
   it is still mapped and open; the next fault in the mapping read an inode
   that was no longer there, which the fault path reported as out of memory
   and killed the browser for (M187). sqlite's journals and base's temporary
   files are the same shape.

   A hold is an open file on this filesystem: open_file_alloc takes one and
   the last open_file_unref gives it back. The counts are in memory, so an
   orphan a power cut leaves on disk is an inode with nlink 0 and no name -
   which nothing creates any other way, every inode being made with nlink 1.
   Reclaiming those at mount is the condition for making an orphan survive a
   crash cleanly, and it waits for a crash that leaves one. */
static uint16_t *holds;
static uint8_t *orphaned;

static int holds_ready(void) {
    if (holds && orphaned) {
        return 1;
    }
    uint16_t *h = (uint16_t *)kmalloc(sizeof(uint16_t) * LEANFS_MAX_INODES);
    uint8_t *o = (uint8_t *)kmalloc(LEANFS_MAX_INODES);
    if (!h || !o) {
        if (h) {
            kfree(h);
        }
        if (o) {
            kfree(o);
        }
        return 0;
    }
    k_memset(h, 0, sizeof(uint16_t) * LEANFS_MAX_INODES);
    k_memset(o, 0, LEANFS_MAX_INODES);
    holds = h;
    orphaned = o;
    return 1;
}

/* A remount is a different set of inodes; a count kept across it would be
   about an inode that may not exist on this disk. */
static void leanfs_forget_holds(void) {
    if (holds) {
        k_memset(holds, 0, sizeof(uint16_t) * LEANFS_MAX_INODES);
    }
    if (orphaned) {
        k_memset(orphaned, 0, LEANFS_MAX_INODES);
    }
}

static int inode_is_held(int idx) {
    return holds && idx >= 0 && (uint32_t)idx < LEANFS_MAX_INODES && holds[idx] > 0;
}

static void orphan_inode(int idx) {
    inodes[idx].nlink = 0;
    mark_inode(idx);
    orphaned[idx] = 1;
}

static void release_inode(int idx) {
    free_inode_blocks(idx);
    inode_changed(idx);
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    mark_inode(idx);
}

void leanfs_handle_hold(int handle) {
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return;
    }
    if (!holds_ready() || holds[handle] == 0xFFFFu) {
        return;
    }
    holds[handle]++;
}

void leanfs_handle_release(int handle) {
    if (!inode_is_held(handle)) {
        return;
    }
    if (--holds[handle] > 0 || !orphaned[handle]) {
        return;
    }
    orphaned[handle] = 0;
    io_begin();
    release_inode(handle);
    save_meta();
}

int leanfs_handle_orphaned(int handle) {
    return holds && handle >= 0 && (uint32_t)handle < LEANFS_MAX_INODES &&
           orphaned[handle];
}

int leanfs_unlink(const char *path) {
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    int idx = directory_lookup(parent, leaf);
    if (!inode_valid(idx) ||
        (inodes[idx].type != LEANFS_TYPE_FILE && inodes[idx].type != LEANFS_TYPE_LINK)) {
        return -1;
    }
    if (inodes[idx].nlink > 1) {
        inodes[idx].nlink--;
        mark_inode(idx);
        if (directory_remove(parent, leaf) < 0) {
            return -1;
        }
        save_meta();
        return 0;
    }
    if (inode_is_held(idx)) {
        orphan_inode(idx);
    } else {
        release_inode(idx);
    }
    if (directory_remove(parent, leaf) < 0) {
        return -1;
    }
    save_meta();
    return 0;
}

int leanfs_link(const char *old_path, const char *new_path) {
    int old_parent, new_parent;
    char old_leaf[LEANFS_MAX_NAME + 1];
    char new_leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(old_path, &old_parent, old_leaf) != 0 ||
        resolve_parent(new_path, &new_parent, new_leaf) != 0) {
        return -1;
    }
    int idx = directory_lookup(old_parent, old_leaf);
    if (!inode_valid(idx)) {
        return -1;
    }
    if (inodes[idx].type == LEANFS_TYPE_DIRECTORY) {
        return -1;
    }
    if (directory_lookup(new_parent, new_leaf) >= 0) {
        return -1;
    }
    if (inodes[idx].nlink == 0xFFFFFFFFu) {
        return -1;
    }
    if (directory_add(new_parent, new_leaf, idx) != 0) {
        return -1;
    }
    inodes[idx].nlink++;
    mark_inode(idx);
    save_meta();
    return 0;
}

uint32_t leanfs_nlink(const char *path) {
    int idx = resolve(path);
    if (!inode_valid(idx)) {
        return 0;
    }
    return inodes[idx].nlink;
}

int leanfs_rename(const char *old_path, const char *new_path) {
    int old_parent, new_parent;
    char old_leaf[LEANFS_MAX_NAME + 1];
    char new_leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(old_path, &old_parent, old_leaf) != 0 ||
        resolve_parent(new_path, &new_parent, new_leaf) != 0) {
        return -1;
    }
    int idx = directory_lookup(old_parent, old_leaf);
    if (!inode_valid(idx)) {
        return -1;
    }
    if (inode_valid(directory_lookup(new_parent, new_leaf))) {
        return -1;
    }
    if (directory_add(new_parent, new_leaf, idx) != 0) {
        return -1;
    }
    if (directory_remove(old_parent, old_leaf) < 0) {
        return -1;
    }
    save_meta();
    return 0;
}

void leanfs_debug_orphan(const char *path) {
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return;
    }
    int idx = directory_lookup(parent, leaf);
    if (!inode_valid(idx)) {
        return;
    }
    directory_remove(parent, leaf);
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

/* fsync(2) and sync(2) promise that what was written is on the disk, and
   nothing more. Until M191 they also marked the filesystem cleanly unmounted -
   on a filesystem that stayed mounted and went on changing, so a crash after
   any program's fsync skipped the mount check - and wrote the superblock
   through to the device to say so. Chromium starting up fsyncs about three
   hundred times, and on a USB stick each of those was a flash erase. */
int leanfs_sync(void) {
    int failed = 0;
    if (journal_active && block_device_commit() != 0) {
        failed = 1;
    }
    if (block_device_flush() != 0) {
        failed = 1;
    }
    return failed ? -1 : 0;
}

void leanfs_unmount_clean(void) {
    block_device_flush();
    sb.state = LEANFS_STATE_CLEAN;
    save_superblock();
    if (journal_active) {
        block_device_checkpoint();
    }
    block_device_flush();
}

/* Called between operations, where the filesystem is consistent: the only
   place a commit may cut the stream of writes. */
void leanfs_transaction_boundary(int from_journal_task) {
    if (journal_active) {
        block_device_commit_if_due(from_journal_task);
    }
}

int leanfs_journal_active(void) {
    return journal_active;
}

int leanfs_rename_replace(const char *old_path, const char *new_path) {
    int old_parent, new_parent;
    char old_leaf[LEANFS_MAX_NAME + 1];
    char new_leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(old_path, &old_parent, old_leaf) != 0 ||
        resolve_parent(new_path, &new_parent, new_leaf) != 0) {
        return -1;
    }
    int idx = directory_lookup(old_parent, old_leaf);
    if (!inode_valid(idx)) {
        return -1;
    }
    int victim = directory_lookup(new_parent, new_leaf);
    if (inode_valid(victim)) {
        if (inodes[victim].type == LEANFS_TYPE_DIRECTORY) {
            return -1;
        }
        if (victim == idx) {
            return 0;
        }
    }

    if (directory_repoint(new_parent, new_leaf, idx) != 0) {
        if (directory_add(new_parent, new_leaf, idx) != 0) {
            return -1;
        }
    } else if (inode_valid(victim)) {
        if (inode_is_held(victim)) {
            orphan_inode(victim);
        } else {
            free_inode_blocks(victim);
            inodes[victim].type = LEANFS_TYPE_FREE;
            inodes[victim].size = 0;
            mark_inode(victim);
        }
    }
    if (directory_remove(old_parent, old_leaf) < 0) {
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
    int idx = directory_lookup(parent, leaf);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_DIRECTORY) {
        return -1;
    }
    if (!directory_is_empty(idx)) {
        return -1;
    }
    free_inode_blocks(idx);
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    mark_inode(idx);
    if (directory_remove(parent, leaf) < 0) {
        return -1;
    }
    save_meta();
    return 0;
}

int leanfs_symlink(const char *path, const char *target) {
    if (!target || !target[0]) {
        return -1;
    }
    size_t tlen = k_strlen(target);
    if (tlen >= LEANFS_MAX_PATH) {
        return -1;
    }
    int parent;
    char leaf[LEANFS_MAX_NAME + 1];
    if (resolve_parent(path, &parent, leaf) != 0) {
        return -1;
    }
    if (inode_valid(directory_lookup(parent, leaf))) {
        return -1;
    }
    int idx = find_free_inode();
    if (idx < 0) {
        return -1;
    }
    k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
    inodes[idx].type = LEANFS_TYPE_LINK;
    inodes[idx].mtime = rtc_now();
    inodes[idx].nlink = 1;
    mark_inode(idx);
    if (inode_write_data(idx, target, tlen) != 0) {
        inodes[idx].type = LEANFS_TYPE_FREE;
        mark_inode(idx);
        return -1;
    }
    if (directory_add(parent, leaf, idx) != 0) {
        free_inode_blocks(idx);
        inodes[idx].type = LEANFS_TYPE_FREE;
        mark_inode(idx);
        return -1;
    }
    save_meta();
    return 0;
}

int64_t leanfs_readlink(const char *path, char *buffer, size_t maxlen) {
    int idx = resolve_nofollow(path);
    if (!inode_valid(idx) || inodes[idx].type != LEANFS_TYPE_LINK) {
        return -1;
    }
    uint32_t n = inodes[idx].size;
    if (n > maxlen) {
        n = (uint32_t)maxlen;
    }
    return inode_pread(idx, buffer, n, 0);
}

int leanfs_lstat(const char *path, leanfs_stat_t *out) {
    int idx = resolve_nofollow(path);
    if (!inode_valid(idx)) {
        return -1;
    }
    out->size = inodes[idx].size;
    out->mtime = inodes[idx].mtime;
    out->is_directory = (inodes[idx].type == LEANFS_TYPE_DIRECTORY) ? 1 : 0;
    out->is_link = (inodes[idx].type == LEANFS_TYPE_LINK) ? 1 : 0;
    out->inode = (uint32_t)idx;
    out->generation = inode_generation[idx];
    return 0;
}

int leanfs_stat(const char *path, leanfs_stat_t *out) {
    int idx = resolve(path);
    if (!inode_valid(idx)) {
        return -1;
    }
    out->size = inodes[idx].size;
    out->mtime = inodes[idx].mtime;
    out->is_directory = inodes[idx].type == LEANFS_TYPE_DIRECTORY;
    out->is_link = 0;
    out->inode = (uint32_t)idx;
    out->generation = inode_generation[idx];
    return 0;
}

int leanfs_handle_stat(int handle, leanfs_stat_t *out) {
    if (!inode_valid(handle)) {
        return -1;
    }
    out->size = inodes[handle].size;
    out->mtime = inodes[handle].mtime;
    out->is_directory = inodes[handle].type == LEANFS_TYPE_DIRECTORY;
    out->is_link = 0;
    out->inode = (uint32_t)handle;
    out->generation = inode_generation[handle];
    return 0;
}

int leanfs_open(const char *path, int create) {
    int idx = resolve(path);
    if (inode_valid(idx)) {
        if (create & LEANFS_OPEN_EXCL) {
            return -1;
        }
        if (inodes[idx].type == LEANFS_TYPE_FILE ||
            inodes[idx].type == LEANFS_TYPE_DIRECTORY) {
            return idx;
        }
        return -1;
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
    inodes[idx].nlink = 1;
    mark_inode(idx);
    if (directory_add(parent, leaf, idx) != 0) {
        inodes[idx].type = LEANFS_TYPE_FREE;
        mark_inode(idx);
        return -1;
    }
    save_meta();
    return idx;
}

int64_t leanfs_handle_read(int handle, void *buffer, size_t length, uint32_t off) {
    io_begin();
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    int64_t n = inode_pread(handle, buffer, length, off);
    return io_failed() ? -1 : n;
}

int64_t leanfs_handle_write(int handle, const void *buffer, size_t length, uint32_t off) {
    io_begin();
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    int64_t n = inode_pwrite(handle, buffer, length, off);
    if (n > 0) {
        save_meta();
    }
    return io_failed() ? -1 : n;
}

uint32_t leanfs_handle_size(int handle) {
    if (!inode_valid(handle)) {
        return 0;
    }
    return inodes[handle].size;
}

static void clear_block_pointer(int idx, uint32_t logical) {
    leanfs_inode_t *inode = &inodes[idx];
    static uint32_t table[LEANFS_INDIRECT_POINTERS];

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
        block_read(sb.data_block + inode->indirect, (uint8_t *)table);
        table[logical] = 0;
        block_write_meta(sb.data_block + inode->indirect, (const uint8_t *)table);
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
    block_read(sb.data_block + inode->dindirect, (uint8_t *)table);
    uint32_t mid = table[outer];
    if (!block_present(mid)) {
        return;
    }
    block_read(sb.data_block + mid, (uint8_t *)table);
    table[inner] = 0;
    block_write_meta(sb.data_block + mid, (const uint8_t *)table);
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

int leanfs_handle_truncate_to(int handle, uint32_t length) {
    if (!inode_valid(handle) || inodes[handle].type != LEANFS_TYPE_FILE) {
        return -1;
    }
    if (length > (uint32_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    uint32_t old = inodes[handle].size;
    inode_changed(handle);
    if (length < old) {
        uint32_t first = (length + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
        uint32_t last = (old + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
        for (uint32_t b = first; b < last; b++) {
            int64_t block_device = map_block(handle, b, 0);
            if (block_device >= 0) {
                bitmap_clear((uint32_t)block_device);
                clear_block_pointer(handle, b);
            }
        }
    }
    inodes[handle].size = length;
    inodes[handle].mtime = rtc_now();
    mark_inode(handle);
    save_meta();
    return 0;
}

static int directory_next(int idx, uint32_t *cookie, leanfs_directory_entry_t *out) {
    uint32_t blocks = directory_nblocks(idx);
    uint32_t position = *cookie;

    while (position / LEANFS_BLOCK_SIZE < blocks) {
        uint32_t b = position / LEANFS_BLOCK_SIZE;
        uint32_t want = position % LEANFS_BLOCK_SIZE;
        if (directory_block_read(idx, b) < 0) {
            return -1;
        }
        uint32_t off = 0;
        while (off < LEANFS_BLOCK_SIZE) {
            leanfs_dirent_t *r = directory_rec(off);
            if (off >= want && r->inode != 0) {
                uint32_t n = r->name_length;
                if (n > LEANFS_MAX_NAME) {
                    return -1;
                }
                out->inode = r->inode;
                out->is_directory = (uint8_t)(r->type == LEANFS_TYPE_DIRECTORY);
                out->is_link = (uint8_t)(r->type == LEANFS_TYPE_LINK);
                k_memcpy(out->name, directory_block + off + LEANFS_DIRENT_HEADER, n);
                out->name[n] = '\0';
                *cookie = b * LEANFS_BLOCK_SIZE + off + r->rec_length;
                return 1;
            }
            off += r->rec_length;
        }
        position = (b + 1) * LEANFS_BLOCK_SIZE;
    }
    *cookie = blocks * LEANFS_BLOCK_SIZE;
    return 0;
}

int leanfs_directory_open(const char *path) {
    int idx = resolve(path);
    return directory_ok(idx) ? idx : -1;
}

int leanfs_readdir_at(int handle, uint32_t *cookie, leanfs_directory_entry_t *out) {
    if (!directory_ok(handle)) {
        return -1;
    }
    return directory_next(handle, cookie, out);
}

int leanfs_readdir(const char *path, uint32_t *cookie, leanfs_directory_entry_t *out) {
    int idx = leanfs_directory_open(path);
    if (idx < 0) {
        return -1;
    }
    return directory_next(idx, cookie, out);
}

size_t leanfs_list(const char *path, char *buffer, size_t maxlen) {
    int idx = resolve(path);
    if (!directory_ok(idx)) {
        return 0;
    }
    size_t written = 0;
    uint32_t cookie = 0;
    leanfs_directory_entry_t e;
    while (directory_next(idx, &cookie, &e) == 1) {
        int child = (int)e.inode;
        int is_directory = inode_valid(child) && inodes[child].type == LEANFS_TYPE_DIRECTORY;
        size_t name_length = k_strlen(e.name);
        size_t need = name_length + (is_directory ? 1u : 0u) + 1u;
        if (written + need > maxlen) {
            break;
        }
        k_memcpy(buffer + written, e.name, name_length);
        written += name_length;
        if (is_directory) {
            buffer[written++] = '/';
        }
        buffer[written++] = '\n';
    }
    return written;
}
