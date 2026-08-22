/* tools/leanfs-put.c
 *
 * Stretch goal: package/build tooling for third-party user programs.
 * Everything through M23 gets onto the disk exactly one way - baked into
 * kernel.bin as an incbin'd blob (kernel/proc/embed_programs.asm) and
 * seeded onto leanfs on first boot (kernel.c) - which means writing a new
 * program has always meant editing the Makefile's USER_PROGRAMS list,
 * embed_programs.asm, and kernel.c's FOR_EACH_EMBEDDED_PROGRAM together.
 * Fine for programs this project ships itself; not something a
 * third-party author should have to do just to try their own program.
 *
 * This is the other way onto the disk: a host-side tool that speaks
 * leanfs's on-disk format directly (kernel/fs/leanfs.c's own layout,
 * duplicated here byte-for-byte - the two structs below MUST stay in
 * sync with that file if the format ever changes) and writes a file
 * straight into an already-built build/os-image.bin's leanfs region,
 * no kernel rebuild involved. kernel.c's own seeding loop already checks
 * `vfs_exists(name)` per program before writing it (not "is the whole
 * filesystem fresh") - so a file this tool puts there survives that loop
 * untouched, and every built-in program still gets seeded normally
 * alongside it. See tools/build-user-program.sh for the other half (how
 * a third party actually compiles a .c file against this project's
 * user-space runtime in the first place) and docs/third-party-programs.md
 * for the end-to-end workflow.
 *
 * Ordinary hosted C - this never runs as part of the OS, so none of the
 * freestanding/no-libc rules elsewhere in this repo apply to it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEANFS_MAGIC 0x3153464Cu
#define LEANFS_START_LBA 2048u
#define LEANFS_MAX_NAME 27
#define LEANFS_DIRECT_BLOCKS 16
#define LEANFS_BLOCK_SIZE 512
#define LEANFS_INDIRECT_POINTERS (LEANFS_BLOCK_SIZE / (int)sizeof(uint32_t))
#define LEANFS_MAX_FILE_SIZE ((LEANFS_DIRECT_BLOCKS + LEANFS_INDIRECT_POINTERS) * LEANFS_BLOCK_SIZE)
#define LEANFS_MAX_INODES 32
#define LEANFS_DATA_BLOCKS 65536u

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
    uint32_t indirect;
} leanfs_inode_t;

#define INODE_TABLE_SECTORS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_SECTORS (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE)

static FILE *img;
static leanfs_superblock_t sb;
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[BITMAP_SECTORS * LEANFS_BLOCK_SIZE];

static void die(const char *msg) {
    fprintf(stderr, "leanfs-put: %s\n", msg);
    exit(1);
}

static void pread_at(uint64_t byte_off, void *buf, size_t len) {
    if (fseeko(img, (off_t)byte_off, SEEK_SET) != 0 || fread(buf, 1, len, img) != len) {
        die("short read from disk image - is it fully built (`make all`)?");
    }
}

static void pwrite_at(uint64_t byte_off, const void *buf, size_t len) {
    if (fseeko(img, (off_t)byte_off, SEEK_SET) != 0 || fwrite(buf, 1, len, img) != len) {
        die("short write to disk image");
    }
}

static uint64_t lba_bytes(uint32_t lba) {
    return (uint64_t)lba * LEANFS_BLOCK_SIZE;
}

static void format_fresh(void) {
    memset(&sb, 0, sizeof(sb));
    sb.magic = LEANFS_MAGIC;
    sb.inode_table_lba = LEANFS_START_LBA + 1;
    sb.inode_table_sectors = INODE_TABLE_SECTORS;
    sb.bitmap_lba = sb.inode_table_lba + INODE_TABLE_SECTORS;
    sb.bitmap_sectors = BITMAP_SECTORS;
    sb.data_lba = sb.bitmap_lba + BITMAP_SECTORS;
    sb.data_blocks = LEANFS_DATA_BLOCKS;
    memset(inodes, 0, sizeof(inodes));
    memset(bitmap, 0, sizeof(bitmap));
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

static int find_inode(const char *name) {
    for (int i = 0; i < LEANFS_MAX_INODES; i++) {
        if (inodes[i].used && strcmp(inodes[i].name, name) == 0) {
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

/* Mirrors kernel/fs/leanfs.c's free_inode_blocks exactly - has to, since
 * it's undoing exactly what that function's on-disk format describes. */
static void free_inode_blocks(leanfs_inode_t *inode) {
    uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
    uint32_t indirect_table[LEANFS_INDIRECT_POINTERS];
    int have_indirect = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        if (b < LEANFS_DIRECT_BLOCKS) {
            bitmap_clear(inode->direct[b]);
        } else {
            if (!have_indirect) {
                pread_at(lba_bytes(sb.data_lba + inode->indirect), indirect_table, sizeof(indirect_table));
                have_indirect = 1;
            }
            bitmap_clear(indirect_table[b - LEANFS_DIRECT_BLOCKS]);
        }
    }
    if (nblocks > LEANFS_DIRECT_BLOCKS) {
        bitmap_clear(inode->indirect);
    }
}

int main(int argc, char **argv) {
    /* Every on-disk field here is a plain little-endian uint32_t, matching
     * both this project's only real target (x86_64) and every plausible
     * dev machine (x86_64 or little-endian arm64/Apple Silicon) - this
     * check exists purely so a port to a big-endian host fails loudly
     * instead of silently writing a corrupt filesystem. */
    uint32_t endian_probe = 1;
    if (*(uint8_t *)&endian_probe != 1) {
        die("this host is big-endian; leanfs's on-disk format is not portable to it as written");
    }

    if (argc != 4) {
        fprintf(stderr, "usage: %s <disk-image> <local-file> <leanfs-name>\n", argv[0]);
        fprintf(stderr, "  Writes local-file into disk-image's leanfs filesystem as leanfs-name,\n");
        fprintf(stderr, "  without touching the kernel build at all. disk-image must already exist\n");
        fprintf(stderr, "  and be at least as large as `make all` produces (build/os-image.bin).\n");
        return 1;
    }
    const char *image_path = argv[1];
    const char *local_path = argv[2];
    const char *leanfs_name = argv[3];

    if (strlen(leanfs_name) > LEANFS_MAX_NAME) {
        die("name too long for leanfs (LEANFS_MAX_NAME = 27)");
    }

    FILE *src = fopen(local_path, "rb");
    if (!src) {
        die("could not open local-file");
    }
    fseeko(src, 0, SEEK_END);
    long len = ftello(src);
    if (len < 0 || (size_t)len > LEANFS_MAX_FILE_SIZE) {
        die("local-file is empty, unreadable, or exceeds LEANFS_MAX_FILE_SIZE (72 KiB)");
    }
    uint8_t *data = malloc((size_t)len);
    if (!data) {
        die("out of memory reading local-file");
    }
    fseeko(src, 0, SEEK_SET);
    if (fread(data, 1, (size_t)len, src) != (size_t)len) {
        die("short read on local-file");
    }
    fclose(src);

    img = fopen(image_path, "r+b");
    if (!img) {
        die("could not open disk-image for read/write - run `make all` first");
    }

    uint8_t sb_buf[LEANFS_BLOCK_SIZE];
    pread_at(lba_bytes(LEANFS_START_LBA), sb_buf, sizeof(sb_buf));
    memcpy(&sb, sb_buf, sizeof(sb));

    if (sb.magic == LEANFS_MAGIC) {
        uint8_t table_buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
        pread_at(lba_bytes(sb.inode_table_lba), table_buf, sizeof(table_buf));
        memcpy(inodes, table_buf, sizeof(inodes));
        pread_at(lba_bytes(sb.bitmap_lba), bitmap, sizeof(bitmap));
    } else {
        format_fresh();
    }

    int idx = find_inode(leanfs_name);
    if (idx >= 0) {
        free_inode_blocks(&inodes[idx]);
    } else {
        idx = find_free_inode();
        if (idx < 0) {
            die("no free inode (leanfs supports at most 32 files total)");
        }
        memset(&inodes[idx], 0, sizeof(inodes[idx]));
        strncpy(inodes[idx].name, leanfs_name, LEANFS_MAX_NAME);
        inodes[idx].used = 1;
    }
    leanfs_inode_t *inode = &inodes[idx];

    uint32_t needed_blocks = (uint32_t)(((uint32_t)len + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE);
    uint32_t indirect_table[LEANFS_INDIRECT_POINTERS];
    int indirect_block = -1;
    if (needed_blocks > LEANFS_DIRECT_BLOCKS) {
        indirect_block = alloc_block();
        if (indirect_block < 0) {
            die("filesystem full (no free data blocks for the indirect table)");
        }
    }
    for (uint32_t b = 0; b < needed_blocks; b++) {
        int blk = alloc_block();
        if (blk < 0) {
            die("filesystem full (no free data blocks)");
        }
        if (b < LEANFS_DIRECT_BLOCKS) {
            inode->direct[b] = (uint32_t)blk;
        } else {
            indirect_table[b - LEANFS_DIRECT_BLOCKS] = (uint32_t)blk;
        }
    }
    inode->indirect = (indirect_block >= 0) ? (uint32_t)indirect_block : 0;
    inode->size = (uint32_t)len;

    if (indirect_block >= 0) {
        uint8_t table_buf[LEANFS_BLOCK_SIZE];
        memset(table_buf, 0, sizeof(table_buf));
        memcpy(table_buf, indirect_table, (needed_blocks - LEANFS_DIRECT_BLOCKS) * sizeof(uint32_t));
        pwrite_at(lba_bytes(sb.data_lba + (uint32_t)indirect_block), table_buf, sizeof(table_buf));
    }

    size_t written = 0;
    for (uint32_t b = 0; b < needed_blocks; b++) {
        uint32_t block = (b < LEANFS_DIRECT_BLOCKS) ? inode->direct[b] : indirect_table[b - LEANFS_DIRECT_BLOCKS];
        uint8_t block_buf[LEANFS_BLOCK_SIZE];
        memset(block_buf, 0, sizeof(block_buf));
        size_t chunk = (size_t)len - written;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        memcpy(block_buf, data + written, chunk);
        pwrite_at(lba_bytes(sb.data_lba + block), block_buf, sizeof(block_buf));
        written += chunk;
    }

    uint8_t table_buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
    memset(table_buf, 0, sizeof(table_buf));
    memcpy(table_buf, inodes, sizeof(inodes));
    pwrite_at(lba_bytes(sb.inode_table_lba), table_buf, sizeof(table_buf));
    pwrite_at(lba_bytes(sb.bitmap_lba), bitmap, sizeof(bitmap));
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &sb, sizeof(sb));
    pwrite_at(lba_bytes(LEANFS_START_LBA), sb_buf, sizeof(sb_buf));

    fclose(img);
    free(data);
    printf("leanfs-put: wrote '%s' (%ld bytes) into %s as '%s'\n", local_path, len, image_path, leanfs_name);
    return 0;
}
