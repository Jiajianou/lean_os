#include "leanfs.h"

#include "drivers/ata.h"
#include "drivers/klog.h"
#include "lib/libk.h"

#define LEANFS_MAGIC     0x3153464Cu /* arbitrary distinctive value, not meant to spell anything when read as bytes */
#define LEANFS_START_LBA 2048u /* 1 MiB in - generously past the boot image; see Makefile's build-time size guard */

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

typedef struct __attribute__((packed)) {
    char name[LEANFS_MAX_NAME + 1];
    uint32_t size;
    uint32_t used;
    uint32_t direct[LEANFS_DIRECT_BLOCKS];
} leanfs_inode_t;

#define INODE_TABLE_SECTORS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_SECTORS       1u
#define MAX_DATA_BLOCKS      (BITMAP_SECTORS * LEANFS_BLOCK_SIZE * 8u)

static leanfs_superblock_t sb;
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[LEANFS_BLOCK_SIZE];

static void save_superblock(void) {
    uint8_t buf[LEANFS_BLOCK_SIZE];
    k_memset(buf, 0, sizeof(buf));
    k_memcpy(buf, &sb, sizeof(sb));
    ata_write_sectors(LEANFS_START_LBA, 1, buf);
}

static void save_inodes(void) {
    uint8_t buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
    k_memset(buf, 0, sizeof(buf));
    k_memcpy(buf, inodes, sizeof(inodes));
    ata_write_sectors(sb.inode_table_lba, (uint8_t)INODE_TABLE_SECTORS, buf);
}

static void save_bitmap(void) {
    ata_write_sectors(sb.bitmap_lba, (uint8_t)BITMAP_SECTORS, bitmap);
}

static void format(void) {
    klog_puts("[fs] no valid leanfs superblock found - formatting fresh\n");

    sb.magic = LEANFS_MAGIC;
    sb.inode_table_lba = LEANFS_START_LBA + 1;
    sb.inode_table_sectors = INODE_TABLE_SECTORS;
    sb.bitmap_lba = sb.inode_table_lba + INODE_TABLE_SECTORS;
    sb.bitmap_sectors = BITMAP_SECTORS;
    sb.data_lba = sb.bitmap_lba + BITMAP_SECTORS;
    sb.data_blocks = MAX_DATA_BLOCKS;
    sb.reserved = 0;

    k_memset(inodes, 0, sizeof(inodes));
    k_memset(bitmap, 0, sizeof(bitmap));

    save_superblock();
    save_inodes();
    save_bitmap();
}

void leanfs_init(void) {
    uint8_t buf[LEANFS_BLOCK_SIZE];
    ata_read_sectors(LEANFS_START_LBA, 1, buf);
    k_memcpy(&sb, buf, sizeof(sb));

    if (sb.magic != LEANFS_MAGIC) {
        format();
    } else {
        uint8_t table_buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
        ata_read_sectors(sb.inode_table_lba, (uint8_t)sb.inode_table_sectors, table_buf);
        k_memcpy(inodes, table_buf, sizeof(inodes));

        ata_read_sectors(sb.bitmap_lba, (uint8_t)sb.bitmap_sectors, bitmap);
    }

    klog_puts("[fs] leanfs ready: data_lba=0x");
    klog_put_hex32(sb.data_lba);
    klog_puts(" data_blocks=0x");
    klog_put_hex32(sb.data_blocks);
    klog_putc('\n');
}

static int find_inode(const char *name) {
    for (int i = 0; i < LEANFS_MAX_INODES; i++) {
        if (inodes[i].used && k_strcmp(inodes[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_inode(void) {
    for (int i = 0; i < LEANFS_MAX_INODES; i++) {
        if (!inodes[i].used) {
            return i;
        }
    }
    return -1;
}

static int bitmap_test(uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bitmap_clear(uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
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

int leanfs_exists(const char *name) {
    return find_inode(name) >= 0;
}

size_t leanfs_list(char *buf, size_t maxlen) {
    size_t written = 0;
    for (int i = 0; i < LEANFS_MAX_INODES; i++) {
        if (!inodes[i].used) {
            continue;
        }
        size_t name_len = k_strlen(inodes[i].name);
        if (written + name_len + 1 > maxlen) {
            break;
        }
        k_memcpy(buf + written, inodes[i].name, name_len);
        written += name_len;
        buf[written++] = '\n';
    }
    return written;
}

int64_t leanfs_read(const char *name, void *buf, size_t maxlen) {
    int idx = find_inode(name);
    if (idx < 0) {
        return -1;
    }
    leanfs_inode_t *inode = &inodes[idx];

    size_t to_copy = inode->size < maxlen ? inode->size : maxlen;
    size_t copied = 0;
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    for (uint32_t b = 0; copied < to_copy; b++) {
        ata_read_sectors(sb.data_lba + inode->direct[b], 1, block_buf);
        size_t chunk = to_copy - copied;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        k_memcpy((uint8_t *)buf + copied, block_buf, chunk);
        copied += chunk;
    }

    return (int64_t)inode->size;
}

int leanfs_write(const char *name, const void *buf, size_t len) {
    if (len > LEANFS_MAX_FILE_SIZE || k_strlen(name) > LEANFS_MAX_NAME) {
        return -1;
    }

    int idx = find_inode(name);
    if (idx < 0) {
        idx = find_free_inode();
        if (idx < 0) {
            return -1;
        }
        k_memset(&inodes[idx], 0, sizeof(inodes[idx]));
        k_strlcpy(inodes[idx].name, name, sizeof(inodes[idx].name));
        inodes[idx].used = 1;
    } else {
        /* Overwriting: free the file's current blocks before reallocating -
         * a fresh write always gets a fresh set, kept simple rather than
         * trying to reuse blocks in place. */
        uint32_t old_blocks = (inodes[idx].size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
        for (uint32_t b = 0; b < old_blocks; b++) {
            bitmap_clear(inodes[idx].direct[b]);
        }
    }

    leanfs_inode_t *inode = &inodes[idx];
    uint32_t needed_blocks = (uint32_t)((len + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE);
    for (uint32_t b = 0; b < needed_blocks; b++) {
        int blk = alloc_block();
        if (blk < 0) {
            for (uint32_t j = 0; j < b; j++) {
                bitmap_clear(inode->direct[j]);
            }
            return -1;
        }
        inode->direct[b] = (uint32_t)blk;
    }
    inode->size = (uint32_t)len;

    size_t written = 0;
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    for (uint32_t b = 0; b < needed_blocks; b++) {
        size_t chunk = len - written;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        k_memset(block_buf, 0, sizeof(block_buf));
        k_memcpy(block_buf, (const uint8_t *)buf + written, chunk);
        ata_write_sectors(sb.data_lba + inode->direct[b], 1, block_buf);
        written += chunk;
    }

    save_inodes();
    save_bitmap();
    return 0;
}
