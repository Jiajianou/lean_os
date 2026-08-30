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
 * leanfs's on-disk format directly and writes a file straight into an
 * already-built build/os-image.bin's leanfs region, no kernel rebuild
 * involved. See tools/build-user-program.sh for the other half (how a
 * third party compiles a .c file against this project's user-space
 * runtime in the first place) and docs/third-party-programs.md for the
 * end-to-end workflow.
 *
 * ---- the drift this file was found in, and what stops it recurring ----
 *
 * This tool duplicates leanfs's on-disk format, and duplication drifts.
 * It had: an inode with a `name` field (M53 moved names into directory
 * records), no `mtime` or `dindirect` (M59 added both), `used` where the
 * format now has `type`, a 32-inode cap the kernel left behind at M53,
 * and no concept of a directory at all - so on a filesystem with a /bin
 * in it, a file this tool wrote was unreachable even when the write
 * "succeeded". It had not worked since M53 and nothing noticed, because
 * nothing runs it: `make preseed` is optional and the boot self-tests do
 * not use it.
 *
 * Two things are different now. The layout comes from the SUPERBLOCK
 * rather than from constants recomputed here - that is what a superblock
 * is for, and it means the tool cannot disagree with the kernel about
 * where the inode table is even if the sizes change again. And every
 * remaining compile-time constant is checked against the superblock at
 * runtime, so a mismatch is a refusal with a message naming the file to
 * fix rather than a corrupt filesystem.
 *
 * The structs below still have to match kernel/fs/leanfs.c byte for
 * byte. There is no way around that in a host program that cannot
 * include a freestanding kernel header - but a mismatch now shows up as
 * "this image's inode table is N bytes per entry, this tool expects M".
 *
 * Ordinary hosted C - this never runs as part of the OS, so none of the
 * freestanding/no-libc rules elsewhere in this repo apply to it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- kernel/fs/leanfs.h, duplicated - see the header comment --------- */
/* "LFS3". Bumped by the kernel whenever the on-disk layout moves (M12
 * 0x3153464C -> M53 0x3253464C -> M59 this one), and the kernel reformats
 * any disk whose magic is not exactly its own - so a stale value here does
 * not produce a diagnosable error, it produces a filesystem this tool wrote
 * and the next boot silently throws away. It must track kernel/fs/leanfs.c. */
#define LEANFS_MAGIC             0x3353464Cu
#define LEANFS_START_LBA         2048u
#define LEANFS_MAX_NAME          27
#define LEANFS_DIRECT_BLOCKS     16
#define LEANFS_BLOCK_SIZE        512
#define LEANFS_INDIRECT_POINTERS (LEANFS_BLOCK_SIZE / (int)sizeof(uint32_t)) /* 128 */
#define LEANFS_DINDIRECT_BLOCKS  (LEANFS_INDIRECT_POINTERS * LEANFS_INDIRECT_POINTERS)
#define LEANFS_MAX_FILE_BLOCKS   (LEANFS_DIRECT_BLOCKS + LEANFS_INDIRECT_POINTERS + LEANFS_DINDIRECT_BLOCKS)
#define LEANFS_MAX_FILE_SIZE     (LEANFS_MAX_FILE_BLOCKS * LEANFS_BLOCK_SIZE)
#define LEANFS_MAX_INODES        192
#define LEANFS_MAX_DIRENTS       LEANFS_MAX_INODES
#define LEANFS_DATA_BLOCKS       65536u

#define LEANFS_TYPE_FREE 0
#define LEANFS_TYPE_FILE 1
#define LEANFS_TYPE_DIR  2
#define ROOT_INODE       0

#define LEANFS_STATE_CLEAN 0u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t bitmap_lba;
    uint32_t bitmap_sectors;
    uint32_t data_lba;
    uint32_t data_blocks;
    uint32_t state;
} leanfs_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint32_t mtime;
    uint32_t direct[LEANFS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t dindirect;
} leanfs_inode_t;

typedef struct __attribute__((packed)) {
    char name[LEANFS_MAX_NAME + 1];
    uint32_t inode;
} leanfs_dirent_t;

#define INODE_TABLE_SECTORS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_SECTORS      (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE)

static FILE *img;
static leanfs_superblock_t sb;
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[BITMAP_SECTORS * LEANFS_BLOCK_SIZE];
static leanfs_dirent_t dirents[LEANFS_MAX_DIRENTS];

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

static void read_block(uint32_t block, void *buf) {
    pread_at(lba_bytes(sb.data_lba + block), buf, LEANFS_BLOCK_SIZE);
}

static void write_block(uint32_t block, const void *buf) {
    pwrite_at(lba_bytes(sb.data_lba + block), buf, LEANFS_BLOCK_SIZE);
}

/* ---- the allocator, matching kernel/fs/leanfs.c exactly --------------- */

static int bitmap_test(uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}
static void bitmap_set(uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}
static void bitmap_clear(uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static uint32_t alloc_block(void) {
    for (uint32_t i = 0; i < sb.data_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return i;
        }
    }
    die("filesystem full (no free data blocks)");
    return 0;
}

/* Block 0 is reserved forever so that a zero pointer can mean "nothing
 * here" - the kernel's own block_present rule, and the reason format()
 * below marks it used before anything else. */
static int block_present(uint32_t block) {
    return block != 0 && block < sb.data_blocks;
}

/* The logical-to-physical map, in the same one-derivation form
 * kernel/fs/leanfs.c's map_block uses and for the same reason: the split
 * between the direct, indirect and double-indirect ranges must not drift
 * between the two implementations. Allocates as it goes. */
static uint32_t map_block(leanfs_inode_t *inode, uint32_t logical) {
    if (logical >= (uint32_t)LEANFS_MAX_FILE_BLOCKS) {
        die("file needs more blocks than leanfs can address");
    }
    if (logical < LEANFS_DIRECT_BLOCKS) {
        if (!block_present(inode->direct[logical])) {
            inode->direct[logical] = alloc_block();
        }
        return inode->direct[logical];
    }

    uint32_t rel = logical - LEANFS_DIRECT_BLOCKS;
    int use_dindirect = rel >= (uint32_t)LEANFS_INDIRECT_POINTERS;
    uint32_t table[LEANFS_INDIRECT_POINTERS];
    uint32_t slot;
    uint32_t table_block;

    if (!use_dindirect) {
        slot = rel;
        if (!block_present(inode->indirect)) {
            inode->indirect = alloc_block();
            memset(table, 0, sizeof(table));
            write_block(inode->indirect, table);
        }
        table_block = inode->indirect;
    } else {
        uint32_t drel = rel - (uint32_t)LEANFS_INDIRECT_POINTERS;
        uint32_t outer = drel / (uint32_t)LEANFS_INDIRECT_POINTERS;
        slot = drel % (uint32_t)LEANFS_INDIRECT_POINTERS;
        if (!block_present(inode->dindirect)) {
            inode->dindirect = alloc_block();
            memset(table, 0, sizeof(table));
            write_block(inode->dindirect, table);
        }
        read_block(inode->dindirect, table);
        if (!block_present(table[outer])) {
            uint32_t inner = alloc_block();
            table[outer] = inner;
            write_block(inode->dindirect, table);
            uint32_t empty[LEANFS_INDIRECT_POINTERS];
            memset(empty, 0, sizeof(empty));
            write_block(inner, empty);
        }
        table_block = table[outer];
    }

    read_block(table_block, table);
    if (!block_present(table[slot])) {
        table[slot] = alloc_block();
        write_block(table_block, table);
    }
    return table[slot];
}

static void free_inode_blocks(leanfs_inode_t *inode) {
    uint32_t nblocks = (inode->size + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE;
    uint32_t table[LEANFS_INDIRECT_POINTERS];

    for (uint32_t b = 0; b < nblocks && b < LEANFS_DIRECT_BLOCKS; b++) {
        if (block_present(inode->direct[b])) {
            bitmap_clear(inode->direct[b]);
        }
    }
    if (block_present(inode->indirect)) {
        read_block(inode->indirect, table);
        for (int i = 0; i < LEANFS_INDIRECT_POINTERS; i++) {
            if (block_present(table[i])) {
                bitmap_clear(table[i]);
            }
        }
        bitmap_clear(inode->indirect);
    }
    if (block_present(inode->dindirect)) {
        uint32_t outer_table[LEANFS_INDIRECT_POINTERS];
        read_block(inode->dindirect, outer_table);
        for (int o = 0; o < LEANFS_INDIRECT_POINTERS; o++) {
            if (!block_present(outer_table[o])) {
                continue;
            }
            read_block(outer_table[o], table);
            for (int i = 0; i < LEANFS_INDIRECT_POINTERS; i++) {
                if (block_present(table[i])) {
                    bitmap_clear(table[i]);
                }
            }
            bitmap_clear(outer_table[o]);
        }
        bitmap_clear(inode->dindirect);
    }
    memset(inode->direct, 0, sizeof(inode->direct));
    inode->indirect = 0;
    inode->dindirect = 0;
    inode->size = 0;
}

/* ---- whole-file read/write over an inode ----------------------------- */

static void inode_read_all(leanfs_inode_t *inode, uint8_t *out, size_t len) {
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    size_t done = 0;
    for (uint32_t b = 0; done < len; b++) {
        uint32_t phys = map_block(inode, b);
        read_block(phys, block_buf);
        size_t chunk = len - done;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        memcpy(out + done, block_buf, chunk);
        done += chunk;
    }
}

static void inode_write_all(leanfs_inode_t *inode, const uint8_t *data, size_t len) {
    free_inode_blocks(inode);
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    size_t done = 0;
    for (uint32_t b = 0; done < len; b++) {
        uint32_t phys = map_block(inode, b);
        size_t chunk = len - done;
        if (chunk > LEANFS_BLOCK_SIZE) {
            chunk = LEANFS_BLOCK_SIZE;
        }
        /* Zero-filled, so the tail of a partly-used final block is not
         * whatever the previous occupant left there. The kernel never
         * reads past `size`, so this is hygiene rather than correctness -
         * but a disk image is a thing people copy around. */
        memset(block_buf, 0, sizeof(block_buf));
        memcpy(block_buf, data + done, chunk);
        write_block(phys, block_buf);
        done += chunk;
    }
    inode->size = (uint32_t)len;
    inode->mtime = (uint32_t)time(NULL);
}

/* ---- directories ----------------------------------------------------- */

static int dir_load(leanfs_inode_t *dir) {
    if (dir->type != LEANFS_TYPE_DIR) {
        die("a path component exists but is not a directory");
    }
    if (dir->size > sizeof(dirents)) {
        die("directory holds more records than there are inodes to name it - corrupt image");
    }
    memset(dirents, 0, sizeof(dirents));
    if (dir->size > 0) {
        inode_read_all(dir, (uint8_t *)dirents, dir->size);
    }
    return (int)(dir->size / sizeof(leanfs_dirent_t));
}

static int dir_lookup(leanfs_inode_t *dir, const char *name) {
    int count = dir_load(dir);
    for (int i = 0; i < count; i++) {
        if (dirents[i].name[0] && strcmp(dirents[i].name, name) == 0) {
            return (int)dirents[i].inode;
        }
    }
    return -1;
}

static int alloc_inode(uint32_t type) {
    for (int i = 0; i < LEANFS_MAX_INODES; i++) {
        if (inodes[i].type == LEANFS_TYPE_FREE) {
            memset(&inodes[i], 0, sizeof(inodes[i]));
            inodes[i].type = type;
            return i;
        }
    }
    fprintf(stderr, "leanfs-put: no free inode (this filesystem holds at most %d)\n",
            LEANFS_MAX_INODES);
    exit(1);
}

/* Adds a record, reusing a slot a removal left free before growing the
 * directory - the same rule kernel/fs/leanfs.c's dir_add follows. */
static void dir_add(leanfs_inode_t *dir, const char *name, int inode_idx) {
    int count = dir_load(dir);
    int slot = -1;
    for (int i = 0; i < count; i++) {
        if (!dirents[i].name[0]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (count >= LEANFS_MAX_DIRENTS) {
            die("directory is full");
        }
        slot = count++;
    }
    memset(&dirents[slot], 0, sizeof(dirents[slot]));
    snprintf(dirents[slot].name, sizeof(dirents[slot].name), "%s", name);
    dirents[slot].inode = (uint32_t)inode_idx;
    inode_write_all(dir, (const uint8_t *)dirents, (size_t)count * sizeof(leanfs_dirent_t));
}

/* Walks `path` and hands back the inode of its parent directory plus the
 * leaf name. Missing intermediate directories are CREATED - `mkdir -p`
 * semantics, which the kernel's own leanfs_mkdir deliberately does not
 * have (it is a syscall, and "was this already here" is a question a
 * caller should be able to ask). A host tool preseeding a freshly
 * formatted image has no such caller and no other way to put a file in
 * /bin, so it makes the directories it needs and says so. */
static int resolve_parent(const char *path, char *leaf_out) {
    if (path[0] != '/') {
        die("leanfs paths are absolute - give a name like /bin/yourprog");
    }
    int at = ROOT_INODE;
    const char *p = path + 1;
    for (;;) {
        const char *slash = strchr(p, '/');
        if (!slash) {
            break;
        }
        size_t n = (size_t)(slash - p);
        if (n == 0 || n > LEANFS_MAX_NAME) {
            die("empty or over-long path component");
        }
        char comp[LEANFS_MAX_NAME + 1];
        memcpy(comp, p, n);
        comp[n] = '\0';
        int next = dir_lookup(&inodes[at], comp);
        if (next < 0) {
            next = alloc_inode(LEANFS_TYPE_DIR);
            dir_add(&inodes[at], comp, next);
            fprintf(stderr, "leanfs-put: created directory %.*s\n",
                    (int)(slash - path), path);
        }
        at = next;
        p = slash + 1;
    }
    if (*p == '\0' || strlen(p) > LEANFS_MAX_NAME) {
        die("empty or over-long final path component");
    }
    snprintf(leaf_out, LEANFS_MAX_NAME + 1, "%s", p);
    return at;
}

/* ---- format, matching the kernel's own ------------------------------- */

static void format_fresh(void) {
    memset(&sb, 0, sizeof(sb));
    sb.magic = LEANFS_MAGIC;
    sb.inode_table_lba = LEANFS_START_LBA + 1;
    sb.inode_table_sectors = (uint32_t)INODE_TABLE_SECTORS;
    sb.bitmap_lba = sb.inode_table_lba + (uint32_t)INODE_TABLE_SECTORS;
    sb.bitmap_sectors = BITMAP_SECTORS;
    sb.data_lba = sb.bitmap_lba + BITMAP_SECTORS;
    sb.data_blocks = LEANFS_DATA_BLOCKS;
    sb.state = LEANFS_STATE_CLEAN;

    memset(inodes, 0, sizeof(inodes));
    memset(bitmap, 0, sizeof(bitmap));
    bitmap[0] |= 1u; /* block 0 reserved forever - see block_present */

    /* The root directory is inode 0 and exists from the moment the
     * filesystem does. Empty: a directory with no entries needs no
     * storage. */
    inodes[ROOT_INODE].type = LEANFS_TYPE_DIR;
    inodes[ROOT_INODE].size = 0;
}

static void save_all(void) {
    uint8_t sb_buf[LEANFS_BLOCK_SIZE];
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &sb, sizeof(sb));
    pwrite_at(lba_bytes(LEANFS_START_LBA), sb_buf, sizeof(sb_buf));

    static uint8_t table_buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
    memset(table_buf, 0, sizeof(table_buf));
    memcpy(table_buf, inodes, sizeof(inodes));
    pwrite_at(lba_bytes(sb.inode_table_lba), table_buf, sizeof(table_buf));

    pwrite_at(lba_bytes(sb.bitmap_lba), bitmap, sizeof(bitmap));
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
        fprintf(stderr, "usage: %s <disk-image> <local-file> <leanfs-path>\n", argv[0]);
        fprintf(stderr, "  Writes local-file into disk-image's leanfs filesystem at leanfs-path,\n");
        fprintf(stderr, "  without touching the kernel build at all. leanfs-path is absolute\n");
        fprintf(stderr, "  (\"/bin/yourprog\"); missing parent directories are created.\n");
        fprintf(stderr, "  disk-image must already exist and be at least as large as `make all`\n");
        fprintf(stderr, "  produces (build/os-image.bin).\n");
        return 1;
    }
    const char *image_path = argv[1];
    const char *local_path = argv[2];
    const char *leanfs_path = argv[3];

    FILE *src = fopen(local_path, "rb");
    if (!src) {
        die("could not open local-file");
    }
    fseeko(src, 0, SEEK_END);
    long len = ftello(src);
    if (len <= 0 || (size_t)len > (size_t)LEANFS_MAX_FILE_SIZE) {
        die("local-file is empty, unreadable, or exceeds LEANFS_MAX_FILE_SIZE (8 MiB)");
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
        /* The layout comes from the image, not from the constants above -
         * see the header comment. What the constants are still needed for
         * is buffer sizing, so each one is checked rather than assumed:
         * an image whose tables are bigger than this build expects is a
         * refusal naming the file to fix, not a truncated read. */
        if (sb.inode_table_sectors > INODE_TABLE_SECTORS) {
            fprintf(stderr,
                    "leanfs-put: this image's inode table is %u sectors and this tool is built "
                    "for %zu.\n            kernel/fs/leanfs.h's LEANFS_MAX_INODES has changed - "
                    "update the copy in this file.\n",
                    sb.inode_table_sectors, (size_t)INODE_TABLE_SECTORS);
            exit(1);
        }
        if (sb.bitmap_sectors > BITMAP_SECTORS || sb.data_blocks > LEANFS_DATA_BLOCKS) {
            die("this image's data region is larger than this tool is built for - "
                "update LEANFS_DATA_BLOCKS in this file from kernel/fs/leanfs.h");
        }
        static uint8_t table_buf[INODE_TABLE_SECTORS * LEANFS_BLOCK_SIZE];
        memset(table_buf, 0, sizeof(table_buf));
        pread_at(lba_bytes(sb.inode_table_lba), table_buf,
                  (size_t)sb.inode_table_sectors * LEANFS_BLOCK_SIZE);
        memcpy(inodes, table_buf, sizeof(inodes));
        memset(bitmap, 0, sizeof(bitmap));
        pread_at(lba_bytes(sb.bitmap_lba), bitmap,
                  (size_t)sb.bitmap_sectors * LEANFS_BLOCK_SIZE);
        if (inodes[ROOT_INODE].type != LEANFS_TYPE_DIR) {
            die("this image has a leanfs superblock but no root directory - corrupt");
        }
    } else if (sb.magic != 0 && sb.magic != 0xFFFFFFFFu) {
        /* Some other leanfs. Not a blank region and not ours: this image
         * was formatted by a kernel whose LEANFS_MAGIC differs from the
         * one duplicated at the top of this file, which means the layout
         * moved and the structs here describe the wrong bytes.
         *
         * Reformatting would "work" - every write below would succeed
         * and report success - and then the next boot would find a magic
         * it does not recognise, reformat, and throw the whole thing
         * away, with nothing anywhere saying why. That is precisely the
         * failure this tool was just fixed out of, so it is an error
         * here rather than a silent one three minutes into a boot. */
        fprintf(stderr,
                "leanfs-put: this image's leanfs magic is 0x%08X and this tool writes 0x%08X.\n"
                "            The on-disk format has moved - update LEANFS_MAGIC and the structs\n"
                "            in tools/leanfs-put.c from kernel/fs/leanfs.c.\n",
                sb.magic, (unsigned)LEANFS_MAGIC);
        exit(1);
    } else {
        format_fresh();
    }

    char leaf[LEANFS_MAX_NAME + 1];
    int parent = resolve_parent(leanfs_path, leaf);

    int idx = dir_lookup(&inodes[parent], leaf);
    if (idx >= 0) {
        if (inodes[idx].type != LEANFS_TYPE_FILE) {
            die("that path already names a directory");
        }
        /* Replaced in place: the directory record already points here, so
         * the name never stops resolving and no record has to change. */
        free_inode_blocks(&inodes[idx]);
    } else {
        idx = alloc_inode(LEANFS_TYPE_FILE);
        dir_add(&inodes[parent], leaf, idx);
    }

    inode_write_all(&inodes[idx], data, (size_t)len);
    save_all();

    fclose(img);
    free(data);
    printf("leanfs-put: wrote %ld bytes to %s as %s\n", len, image_path, leanfs_path);
    return 0;
}
