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
 * ---- M93 (second attempt): a tree, not a file ------------------------
 *
 * M93's fourth bullet asked for "a host-side image builder... a tool that
 * writes a leanfs image from a host directory", on the grounds that
 * `kernel/proc/embed_programs.asm` is the only road onto this disk and a
 * source tree is not a thing that can be `incbin`'d into a kernel.
 *
 * That is a second mode of this program rather than a second program.
 * Everything a tree needs - the allocator, the block map, the directory
 * records, the format - is already here and already correct, and Q3 exists
 * because a second copy of that code drifted from the first. A new
 * `leanfs-mkimage.c` would have been a third.
 *
 *     leanfs-put -r <image> <host-dir> <leanfs-dir>
 *
 * What the tree mode adds beyond a loop over the file mode, each because a
 * real source tree contains one and the file mode got it wrong:
 *
 *   - empty files. `long len <= 0` was an error ("empty, unreadable"),
 *     and CPython's tree alone has hundreds of empty __init__.py.
 *   - symbolic links, stored as links rather than followed. A tarball
 *     that unpacks to a link and an image that has a second copy of the
 *     file are different trees, and the difference is invisible until
 *     something writes through one of them.
 *   - hard links. Two names for one inode is M93's own new field and a
 *     source tree is where it is first true of somebody else's data.
 *   - the host's mtime rather than this tool's clock, so an image built
 *     twice from one tree is the same image.
 *   - a rolling allocator hint. See alloc_block: the from-zero scan was
 *     invisible at thirty files and quadratic at a hundred thousand.
 *
 * And it writes /.image-manifest: what the host put there, in numbers, so
 * the machine can walk the tree it was given and say whether it agrees.
 * See kernel.c's M93 image-manifest self-test, which is the other half of
 * this and the only one that proves the image is readable from inside.
 *
 * Ordinary hosted C - this never runs as part of the OS, so none of the
 * freestanding/no-libc rules elsewhere in this repo apply to it.
 */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* M89: a symlink made from a string needs a timestamp from somewhere */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- Q3: one definition of the format, not two ----------------------
 *
 * Everything between this comment and `static FILE *img` used to be a
 * hand-copied duplicate of the superblock, the inode and the directory
 * record, under a comment reading "The structs below still have to match
 * kernel/fs/leanfs.c byte for byte."
 *
 * They did not. This tool was still on M81's format - magic "LFS4",
 * version 4, 512-byte blocks, 8192 inodes, a 32 MiB data region - when
 * M93 moved the kernel to "LFS5" with 4096-byte blocks, 131072 inodes and
 * 2 GiB of data. The tool wrote a perfectly well-formed image of the
 * wrong format, and the kernel's own recovery path then reformatted it on
 * the next boot without complaint. That is precisely the failure the old
 * comment predicted: "a stale value here does not produce a diagnosable
 * error, it produces a filesystem this tool wrote and the next boot
 * silently throws away."
 *
 * kernel/fs/leanfs_format.h is now the single definition and this file
 * includes it. There is nothing left to keep in sync. */
#include "../kernel/fs/leanfs_format.h"

static FILE *img;
static leanfs_superblock_t sb;
static leanfs_inode_t inodes[LEANFS_MAX_INODES];
static uint8_t bitmap[BITMAP_BLOCKS * LEANFS_BLOCK_SIZE];
static uint8_t dir_block[LEANFS_BLOCK_SIZE]; /* M81: one directory block at a time */

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

/* Q3: a leanfs block is 4096 bytes and a disk sector is 512, and until
 * M93 they were the same number - which is why every name in this file
 * said "lba" while every value was a block index. They are different
 * units now, so this takes the unit it is given: block_bytes for
 * leanfs's own addressing, and LEANFS_START_BLOCK rather than
 * LEANFS_START_LBA wherever the superblock is reached. */
static uint64_t block_bytes(uint32_t block) {
    return (uint64_t)block * LEANFS_BLOCK_SIZE;
}

static void read_block(uint32_t block, void *buf) {
    pread_at(block_bytes(sb.data_block + block), buf, LEANFS_BLOCK_SIZE);
}

static void write_block(uint32_t block, const void *buf) {
    pwrite_at(block_bytes(sb.data_block + block), buf, LEANFS_BLOCK_SIZE);
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

/* M93 (second attempt): where the last allocation came from.
 *
 * This scan started at block 0 on every call, which is one pass over a
 * 64 KiB bitmap per block allocated - invisible for the thirty files this
 * tool was written to write, and quadratic for a tree. Writing a 40000-file
 * tree spent almost all of its time here.
 *
 * The hint is not a free-list and does not try to be: it is the block
 * after the last one handed out, and the second pass from zero is what
 * keeps it correct rather than merely fast. An image that already has
 * holes in it (this tool run twice, or a booted machine's disk) allocates
 * exactly as it did before, one pass later. */
static uint32_t alloc_hint;

static uint32_t alloc_block(void) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0) ? alloc_hint : 0;
        for (uint32_t i = start; i < sb.data_blocks; i++) {
            if (!bitmap_test(i)) {
                bitmap_set(i);
                alloc_hint = i + 1;
                return i;
            }
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

/* M81: inode_read_all lived here, and was the mirror of inode_write_all
 * below. Its only caller was the old whole-directory dir_load, which
 * variable-length records replaced with a block-at-a-time walk - so it
 * became genuinely uncalled rather than merely half of a pair, and is
 * gone rather than kept behind a pragma. inode_write_all stays because a
 * file's contents are still written in one shot.
 */

static void inode_write_all(leanfs_inode_t *inode, const uint8_t *data, size_t len,
                            uint32_t mtime) {
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
    /* M93 (second attempt): from the caller, not from this tool's clock -
     * see dir_add for the two-images-a-second-apart failure that found
     * the last of these. */
    inode->mtime = mtime;
}

/* ---- directories ----------------------------------------------------- */

/* M81: the block-at-a-time directory, matching kernel/fs/leanfs.c. See
 * dir_add there for the invariants; this is the same code with the
 * kernel's ata_* calls replaced by this tool's read_block/write_block and
 * without the insert hint, which is an optimization for a machine filling
 * a directory rather than for a tool writing thirty files once. */

static leanfs_dirent_t *dir_rec(uint32_t off) {
    return (leanfs_dirent_t *)(void *)(dir_block + off);
}

static int dir_block_valid(void) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        /* Bounds-check the header before reading it - see the same guard
         * in kernel/fs/leanfs.c for why this one function needs it. */
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

static void dir_block_init(void) {
    memset(dir_block, 0, LEANFS_BLOCK_SIZE);
    dir_rec(0)->rec_len = (uint16_t)LEANFS_BLOCK_SIZE;
}

static uint32_t dir_nblocks(const leanfs_inode_t *dir) {
    return dir->size / LEANFS_BLOCK_SIZE;
}

/* Reads directory block `logical`. Unlike the kernel's, this only ever
 * runs against an image this tool or the kernel wrote, so a block that
 * does not validate is a corrupt image and worth dying on rather than
 * working around. */
static void dir_block_read(leanfs_inode_t *dir, uint32_t logical) {
    read_block(map_block(dir, logical), dir_block);
    if (!dir_block_valid()) {
        die("a directory block does not parse - corrupt image");
    }
}

static int32_t dir_block_find(const char *name, uint32_t name_len) {
    uint32_t off = 0;
    while (off < LEANFS_BLOCK_SIZE) {
        leanfs_dirent_t *r = dir_rec(off);
        if (r->inode != 0 && r->name_len == name_len &&
            memcmp(dir_block + off + LEANFS_DIRENT_HDR, name, name_len) == 0) {
            return (int32_t)off;
        }
        off += r->rec_len;
    }
    return -1;
}

static int dir_block_place(const char *name, uint32_t name_len, int inode_idx, uint32_t type) {
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
                n->type = (uint8_t)type;
                memcpy(dir_block + place_at + LEANFS_DIRENT_HDR, name, name_len);
                return 1;
            }
            off += r->rec_len;
        }
    }
    return 0;
}

static int dir_lookup(leanfs_inode_t *dir, const char *name) {
    if (dir->type != LEANFS_TYPE_DIR) {
        die("a path component exists but is not a directory");
    }
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t blocks = dir_nblocks(dir);
    for (uint32_t b = 0; b < blocks; b++) {
        dir_block_read(dir, b);
        int32_t off = dir_block_find(name, name_len);
        if (off >= 0) {
            return (int)dir_rec((uint32_t)off)->inode;
        }
    }
    return -1;
}

/* The same hint as alloc_block's, for the same reason and with the same
 * second pass. 131072 inodes scanned from zero per file is the other half
 * of what made a tree quadratic. */
static uint32_t inode_hint;

static int alloc_inode(uint32_t type) {
    /* Q3: LEANFS_MAX_INODES is unsigned in the shared header (it is
     * 131072u), which the old private copy was not. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0) ? inode_hint : 0;
        for (uint32_t i = start; i < LEANFS_MAX_INODES; i++) {
            if (inodes[i].type == LEANFS_TYPE_FREE) {
                memset(&inodes[i], 0, sizeof(inodes[i]));
                inodes[i].type = type;
                inodes[i].nlink = 1;
                inode_hint = i + 1;
                return (int)i;
            }
        }
    }
    fprintf(stderr, "leanfs-put: no free inode (this filesystem holds at most %d)\n",
            LEANFS_MAX_INODES);
    exit(1);
}

/* M89: repoints an existing name at a different inode, and updates the
 * record's cached type with it.
 *
 * Needed because the multi-call binary's command names collide with this
 * project's own programs - /bin/ls is a lean_os ELF before it is a link
 * to /bin/toybox - and M89's third bullet decided which one wins. The
 * record carries the type as well as the inode number (see
 * leanfs_dirent_t), so changing one without the other would leave a
 * directory that lists a link as a regular file.
 *
 * Returns 0 if the name was not there. */
static int dir_repoint(leanfs_inode_t *dir, const char *name, int inode_idx) {
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t blocks = dir_nblocks(dir);
    for (uint32_t b = 0; b < blocks; b++) {
        dir_block_read(dir, b);
        int32_t off = dir_block_find(name, name_len);
        if (off >= 0) {
            leanfs_dirent_t *r = dir_rec((uint32_t)off);
            r->inode = (uint32_t)inode_idx;
            r->type = (uint8_t)inodes[inode_idx].type;
            write_block(map_block(dir, b), dir_block);
            return 1;
        }
    }
    return 0;
}

/* Adds a record, reusing a hole before growing the directory - the same
 * rule kernel/fs/leanfs.c's dir_add follows. */
static void dir_add(leanfs_inode_t *dir, const char *name, int inode_idx) {
    if (dir->type != LEANFS_TYPE_DIR) {
        die("cannot add an entry to something that is not a directory");
    }
    uint32_t name_len = (uint32_t)strlen(name);
    if (name_len == 0 || name_len > LEANFS_MAX_NAME) {
        die("empty or over-long name");
    }
    uint32_t type = inodes[inode_idx].type;

    /* M93 (second attempt): the last block first.
     *
     * The comment above this section said the kernel's insert hint was
     * "an optimization for a machine filling a directory rather than for
     * a tool writing thirty files once". A tree mode makes this tool the
     * thing filling a directory, and a scan from block 0 per entry is
     * quadratic in the size of the directory - which a source tree's
     * flat leaf directories reach several thousand of.
     *
     * The full scan stays as the fallback rather than being replaced,
     * because it is what reuses a hole left by a previous run of this
     * tool. A freshly built tree has no holes and never gets there. */
    uint32_t blocks = dir_nblocks(dir);
    if (blocks > 0) {
        dir_block_read(dir, blocks - 1);
        if (dir_block_place(name, name_len, inode_idx, type)) {
            write_block(map_block(dir, blocks - 1), dir_block);
            return;
        }
    }
    for (uint32_t b = 0; b + 1 < blocks; b++) {
        dir_block_read(dir, b);
        if (dir_block_place(name, name_len, inode_idx, type)) {
            write_block(map_block(dir, b), dir_block);
            return;
        }
    }
    dir_block_init();
    if (!dir_block_place(name, name_len, inode_idx, type)) {
        die("a name did not fit an empty directory block - impossible");
    }
    write_block(map_block(dir, blocks), dir_block);
    dir->size += LEANFS_BLOCK_SIZE;
    /* M93 (second attempt): no `dir->mtime = time(NULL)` here any more.
     *
     * It was the last piece of wall-clock in this tool, and it was found
     * the way this kind of thing should be: two images built from one
     * tree a second apart failed to compare equal, at byte 4198409, which
     * is the root inode's mtime field. A directory's mtime now comes from
     * the host directory it was made from, set where it is created.
     *
     * What that costs is that putting a single file into an existing
     * image no longer touches its parent's mtime. That is the right way
     * round: this tool is not a process on the machine and the machine's
     * own dir_add still stamps the clock, so a directory's mtime keeps
     * meaning "when something on this machine last changed it". */
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

/* ---- M93 (second attempt): a host directory tree ---------------------- */

/* A file, block at a time, straight from the host's descriptor.
 *
 * inode_write_all above takes the whole file in memory, which was fine
 * when the largest thing this tool wrote was a 200 KiB program and is not
 * fine now: this filesystem holds files up to four gigabytes and a source
 * tarball has some tens of megabytes in it. Nothing here holds more than
 * one block.
 *
 * `mtime` comes from the host's stat rather than from time(NULL), so that
 * building an image twice from the same tree produces the same image.
 * That is worth more than it looks - it is what lets a test compare two
 * builds rather than only checking that one of them parses. */
static void inode_write_stream(leanfs_inode_t *inode, FILE *src, uint64_t len,
                               uint32_t mtime, const char *what, uint32_t *hash) {
    free_inode_blocks(inode);
    if (len > LEANFS_MAX_FILE_SIZE) {
        fprintf(stderr, "leanfs-put: %s is %llu bytes; leanfs holds at most %u\n",
                what, (unsigned long long)len, (unsigned)LEANFS_MAX_FILE_SIZE);
        exit(1);
    }
    uint8_t block_buf[LEANFS_BLOCK_SIZE];
    uint64_t done = 0;
    for (uint32_t b = 0; done < len; b++) {
        size_t chunk = (size_t)((len - done > LEANFS_BLOCK_SIZE)
                                ? LEANFS_BLOCK_SIZE : (len - done));
        /* Zeroed first: the tail of a partly-used final block is not
         * whatever the previous occupant left there. */
        memset(block_buf, 0, sizeof(block_buf));
        if (fread(block_buf, 1, chunk, src) != chunk) {
            fprintf(stderr, "leanfs-put: short read on %s\n", what);
            exit(1);
        }
        if (hash) {
            *hash = leanfs_fnv1a(*hash, block_buf, chunk);
        }
        write_block(map_block(inode, b), block_buf);
        done += chunk;
    }
    inode->size = (uint32_t)len;
    inode->mtime = mtime;
}

/* ---- the same file, reached by a second name -------------------------
 *
 * A tarball that unpacks with hard links in it - and GCC's does - would
 * otherwise become an image with two copies of the data and two inodes,
 * which is a different tree that happens to read the same. M93 added
 * `nlink` for exactly this and this is the first thing that is somebody
 * else's data rather than a self-test's.
 *
 * Keyed on (st_dev, st_ino), which is what identifies a file on the host
 * regardless of how many names it has. Open addressing, because the
 * alternative at a hundred thousand files is a linear scan per file and
 * this milestone has already found two of those. */
#define HL_SLOTS 262144u /* a power of two, and twice LEANFS_MAX_INODES */
typedef struct {
    int      used;
    uint64_t dev;
    uint64_t ino;
    int      inode;
} hardlink_slot_t;
static hardlink_slot_t hardlinks[HL_SLOTS];

static uint32_t hl_hash(uint64_t dev, uint64_t ino) {
    uint64_t h = dev * 0x9E3779B97F4A7C15ull ^ (ino + 0x165667B19E3779F9ull);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return (uint32_t)(h & (HL_SLOTS - 1));
}

static int hl_lookup(uint64_t dev, uint64_t ino) {
    uint32_t i = hl_hash(dev, ino);
    for (uint32_t probe = 0; probe < HL_SLOTS; probe++) {
        hardlink_slot_t *s = &hardlinks[(i + probe) & (HL_SLOTS - 1)];
        if (!s->used) {
            return -1;
        }
        if (s->dev == dev && s->ino == ino) {
            return s->inode;
        }
    }
    return -1;
}

static void hl_remember(uint64_t dev, uint64_t ino, int inode) {
    uint32_t i = hl_hash(dev, ino);
    for (uint32_t probe = 0; probe < HL_SLOTS; probe++) {
        hardlink_slot_t *s = &hardlinks[(i + probe) & (HL_SLOTS - 1)];
        if (!s->used) {
            s->used = 1;
            s->dev = dev;
            s->ino = ino;
            s->inode = inode;
            return;
        }
    }
    die("the hard-link table filled - more distinct files than leanfs has inodes");
}

/* What went in, so that the machine can be asked whether it agrees. */
static struct {
    uint64_t files;      /* distinct regular-file inodes written */
    uint64_t dirs;
    uint64_t links;
    uint64_t hardlinks;  /* extra names for a file already written */
    uint64_t names;      /* regular-file NAMES: files + hardlinks */
    uint64_t bytes;      /* summed over names, so a hard-linked file counts twice */
    uint64_t skipped;
    uint32_t deepest;
    /* The wrapping sum of one FNV-1a per name - see leanfs_fnv1a in the
     * format header for why it is a sum rather than a running hash. */
    uint32_t hash;
} tally;

/* One name's contribution: its path within the tree, then what it holds.
 * The path is in so that two files with equal contents under different
 * names cannot swap without the total moving. */
static uint32_t name_hash_start(const char *leanfs_path, const char *tree_root) {
    size_t skip = strlen(tree_root);
    const char *rel = leanfs_path + skip;
    while (*rel == '/') {
        rel++;
    }
    return leanfs_fnv1a(LEANFS_FNV1A_INIT, rel, strlen(rel));
}

/* The tree this run is writing, so name_hash_start can make a path
 * relative without threading it through every frame of the walk. */
static const char *tree_root;
static uint32_t tree_mtime;

/* Deep enough for any source tree anybody unpacks and shallow enough that
 * the two path buffers per level stay a bounded cost. GCC's own tree is
 * eleven deep; CPython's is nine. */
#define TREE_MAX_DEPTH 48

static void put_tree(const char *host_dir, int parent, const char *at, uint32_t depth) {
    if (depth > TREE_MAX_DEPTH) {
        fprintf(stderr, "leanfs-put: %s is more than %d directories deep\n",
                at, TREE_MAX_DEPTH);
        exit(1);
    }
    if (depth > tally.deepest) {
        tally.deepest = depth;
    }

    struct dirent **names = NULL;
    /* scandir with alphasort rather than readdir in whatever order the
     * host's filesystem hands entries back: an image built twice from one
     * tree should be the same image, and inode allocation follows this
     * order. */
    int n = scandir(host_dir, &names, NULL, alphasort);
    if (n < 0) {
        fprintf(stderr, "leanfs-put: cannot read directory %s: %s\n",
                host_dir, strerror(errno));
        exit(1);
    }

    for (int i = 0; i < n; i++) {
        const char *name = names[i]->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            free(names[i]);
            continue;
        }
        if (strlen(name) > LEANFS_MAX_NAME) {
            fprintf(stderr, "leanfs-put: %s/%s: name is longer than leanfs's %d bytes\n",
                    host_dir, name, LEANFS_MAX_NAME);
            exit(1);
        }

        char host_path[LEANFS_MAX_PATH];
        char leanfs_path[LEANFS_MAX_PATH];
        if (snprintf(host_path, sizeof(host_path), "%s/%s", host_dir, name) >=
                (int)sizeof(host_path) ||
            snprintf(leanfs_path, sizeof(leanfs_path), "%s/%s", at, name) >=
                (int)sizeof(leanfs_path)) {
            fprintf(stderr, "leanfs-put: %s/%s: path longer than %d bytes\n",
                    host_dir, name, LEANFS_MAX_PATH);
            exit(1);
        }

        struct stat st;
        /* lstat, not stat: a symlink is stored as a symlink. Following it
         * here would put a second copy of the target in the image and
         * lose the fact that the tree had a link in it at all. */
        if (lstat(host_path, &st) != 0) {
            fprintf(stderr, "leanfs-put: cannot stat %s: %s\n", host_path, strerror(errno));
            exit(1);
        }
        uint32_t mtime = (uint32_t)st.st_mtime;

        if (S_ISDIR(st.st_mode)) {
            int idx = alloc_inode(LEANFS_TYPE_DIR);
            inodes[idx].mtime = mtime;
            dir_add(&inodes[parent], name, idx);
            tally.dirs++;
            put_tree(host_path, idx, leanfs_path, depth + 1);
        } else if (S_ISLNK(st.st_mode)) {
            char target[LEANFS_MAX_PATH];
            ssize_t tlen = readlink(host_path, target, sizeof(target) - 1);
            if (tlen < 0) {
                fprintf(stderr, "leanfs-put: cannot read the link %s: %s\n",
                        host_path, strerror(errno));
                exit(1);
            }
            int idx = alloc_inode(LEANFS_TYPE_LINK);
            dir_add(&inodes[parent], name, idx);
            /* A link's target lives in its data blocks exactly as a file's
             * contents do, with `size` the target's length - see
             * LEANFS_TYPE_LINK in the format header. */
            inode_write_all(&inodes[idx], (const uint8_t *)target, (size_t)tlen, mtime);
            tally.links++;
            tally.hash += leanfs_fnv1a(name_hash_start(leanfs_path, tree_root),
                                       target, (size_t)tlen);
        } else if (S_ISREG(st.st_mode)) {
            int existing = -1;
            if (st.st_nlink > 1) {
                existing = hl_lookup((uint64_t)st.st_dev, (uint64_t)st.st_ino);
            }
            if (existing >= 0) {
                if (inodes[existing].nlink == 0xFFFFFFFFu) {
                    die("a file has more names than leanfs can count");
                }
                inodes[existing].nlink++;
                dir_add(&inodes[parent], name, existing);
                tally.hardlinks++;
                tally.names++;
                tally.bytes += (uint64_t)st.st_size;
                /* Read again rather than remembered: a second name holds
                 * the same bytes and the machine walking this tree will
                 * read them again too, so the two sides count the same
                 * thing. Rare enough that the second read is not worth
                 * avoiding. */
                {
                    FILE *again = fopen(host_path, "rb");
                    if (!again) {
                        fprintf(stderr, "leanfs-put: cannot re-open %s: %s\n",
                                host_path, strerror(errno));
                        exit(1);
                    }
                    uint32_t h = name_hash_start(leanfs_path, tree_root);
                    uint8_t buf[LEANFS_BLOCK_SIZE];
                    size_t got;
                    while ((got = fread(buf, 1, sizeof(buf), again)) > 0) {
                        h = leanfs_fnv1a(h, buf, got);
                    }
                    fclose(again);
                    tally.hash += h;
                }
            } else {
                FILE *src = fopen(host_path, "rb");
                if (!src) {
                    fprintf(stderr, "leanfs-put: cannot open %s: %s\n",
                            host_path, strerror(errno));
                    exit(1);
                }
                int idx = alloc_inode(LEANFS_TYPE_FILE);
                dir_add(&inodes[parent], name, idx);
                /* Zero-length is a file, not an error. The single-file mode
                 * refused one for eight milestones ("local-file is empty,
                 * unreadable") and a tree full of empty __init__.py is
                 * exactly where that stops being a plausible check. */
                uint32_t h = name_hash_start(leanfs_path, tree_root);
                inode_write_stream(&inodes[idx], src, (uint64_t)st.st_size, mtime,
                                   host_path, &h);
                fclose(src);
                if (st.st_nlink > 1) {
                    hl_remember((uint64_t)st.st_dev, (uint64_t)st.st_ino, idx);
                }
                tally.files++;
                tally.names++;
                tally.bytes += (uint64_t)st.st_size;
                tally.hash += h;
            }
        } else {
            /* A fifo, a socket, a device node. leanfs has three types and
             * none of them is any of these, so this is reported rather
             * than approximated with an empty file - a tree that silently
             * gained a regular file where a fifo was is a tree that
             * differs from the one on the host. */
            fprintf(stderr, "leanfs-put: skipping %s (not a file, directory or symlink)\n",
                    host_path);
            tally.skipped++;
        }
        free(names[i]);
    }
    free(names);
}

/* /.image-manifest - what the host put on this disk, in numbers.
 *
 * This is the half of the milestone's test that the machine can grade.
 * A host-side comparison proves the image parses on the host, which is
 * the weaker of the two claims available; the kernel walking the same
 * tree and arriving at these numbers proves the image is readable by the
 * thing it was built for. See kernel.c's M93 image-manifest self-test.
 *
 * Deliberately a file rather than a superblock field: it describes one
 * tree that one run of this tool wrote, which is not a property of the
 * filesystem, and a format change to carry it would be a format change
 * for a test's benefit. */
static void write_manifest(const char *at) {
    char text[1024];
    int len = snprintf(text, sizeof(text),
                       "leanfs-image-manifest 1\n"
                       "tree %s\n"
                       "dirs %llu\n"
                       "names %llu\n"
                       "links %llu\n"
                       "bytes %llu\n"
                       "depth %u\n"
                       "hash %u\n",
                       at,
                       (unsigned long long)tally.dirs,
                       (unsigned long long)tally.names,
                       (unsigned long long)tally.links,
                       (unsigned long long)tally.bytes,
                       tally.deepest,
                       tally.hash);
    if (len < 0 || len >= (int)sizeof(text)) {
        die("the manifest did not fit its buffer");
    }

    char leaf[LEANFS_MAX_NAME + 1];
    int parent = resolve_parent("/.image-manifest", leaf);
    int idx = dir_lookup(&inodes[parent], leaf);
    if (idx < 0) {
        idx = alloc_inode(LEANFS_TYPE_FILE);
        dir_add(&inodes[parent], leaf, idx);
    } else if (inodes[idx].type != LEANFS_TYPE_FILE) {
        die("/.image-manifest exists and is not a regular file");
    }
    /* The tree's own mtime, so that an image built twice from one tree is
     * the same image down to this file. 0 would have been the other
     * option and it means "no clock on this machine" (see the inode's own
     * note), which is not true here. */
    inode_write_all(&inodes[idx], (const uint8_t *)text, (size_t)len, tree_mtime);
}

/* ---- format, matching the kernel's own ------------------------------- */

static void format_fresh(void) {
    memset(&sb, 0, sizeof(sb));
    sb.magic = LEANFS_MAGIC;
    sb.version = LEANFS_VERSION;
    sb.inode_table_block = LEANFS_START_BLOCK + 1;
    sb.inode_table_blocks = (uint32_t)INODE_TABLE_BLOCKS;
    sb.bitmap_block = sb.inode_table_block + (uint32_t)INODE_TABLE_BLOCKS;
    sb.bitmap_blocks_field = BITMAP_BLOCKS;
    sb.data_block = sb.bitmap_block + BITMAP_BLOCKS;
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
    inodes[ROOT_INODE].nlink = 1; /* M93 - the root is nobody's child and still has one name */
}

static void save_all(void) {
    uint8_t sb_buf[LEANFS_BLOCK_SIZE];
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &sb, sizeof(sb));
    pwrite_at(block_bytes(LEANFS_START_BLOCK), sb_buf, sizeof(sb_buf));

    static uint8_t table_buf[INODE_TABLE_BLOCKS * LEANFS_BLOCK_SIZE];
    memset(table_buf, 0, sizeof(table_buf));
    memcpy(table_buf, inodes, sizeof(inodes));
    pwrite_at(block_bytes(sb.inode_table_block), table_buf, sizeof(table_buf));

    pwrite_at(block_bytes(sb.bitmap_block), bitmap, sizeof(bitmap));
}

/* Opens the image and loads its superblock, inode table and bitmap -
 * formatting a blank region rather than refusing it. Factored out of main
 * when the tree mode arrived, because both modes need exactly this and a
 * second copy of the geometry checks below is the kind of duplication Q3
 * was spent removing. */
static void image_open(const char *image_path) {
    img = fopen(image_path, "r+b");
    if (!img) {
        die("could not open disk-image for read/write - run `make all` first");
    }

    uint8_t sb_buf[LEANFS_BLOCK_SIZE];
    pread_at(block_bytes(LEANFS_START_BLOCK), sb_buf, sizeof(sb_buf));
    memcpy(&sb, sb_buf, sizeof(sb));

    if (sb.magic == LEANFS_MAGIC) {
        /* The layout comes from the image, not from the constants above -
         * see the header comment. What the constants are still needed for
         * is buffer sizing, so each one is checked rather than assumed:
         * an image whose tables are bigger than this build expects is a
         * refusal naming the file to fix, not a truncated read. */
        if (sb.inode_table_blocks > INODE_TABLE_BLOCKS) {
            fprintf(stderr,
                    "leanfs-put: this image's inode table is %u blocks and this tool is built "
                    "for %zu.\n            The image predates this build's "
                    "kernel/fs/leanfs_format.h - rebuild the image.\n",
                    sb.inode_table_blocks, (size_t)INODE_TABLE_BLOCKS);
            exit(1);
        }
        if (sb.bitmap_blocks_field > BITMAP_BLOCKS || sb.data_blocks > LEANFS_DATA_BLOCKS) {
            die("this image's data region is larger than this tool is built for - "
                "the image predates this build's kernel/fs/leanfs_format.h");
        }
        static uint8_t table_buf[INODE_TABLE_BLOCKS * LEANFS_BLOCK_SIZE];
        memset(table_buf, 0, sizeof(table_buf));
        pread_at(block_bytes(sb.inode_table_block), table_buf,
                  (size_t)sb.inode_table_blocks * LEANFS_BLOCK_SIZE);
        memcpy(inodes, table_buf, sizeof(inodes));
        memset(bitmap, 0, sizeof(bitmap));
        pread_at(block_bytes(sb.bitmap_block), bitmap,
                  (size_t)sb.bitmap_blocks_field * LEANFS_BLOCK_SIZE);
        if (inodes[ROOT_INODE].type != LEANFS_TYPE_DIR) {
            die("this image has a leanfs superblock but no root directory - corrupt");
        }
    } else if (sb.magic != 0 && sb.magic != 0xFFFFFFFFu) {
        /* Some other leanfs. Not a blank region and not ours: this image
         * was formatted by a kernel whose LEANFS_MAGIC differs from the
         * one this tool is built against, which means the layout moved
         * and the structs here describe the wrong bytes.
         *
         * Reformatting would "work" - every write below would succeed
         * and report success - and then the next boot would find a magic
         * it does not recognise, reformat, and throw the whole thing
         * away, with nothing anywhere saying why. That is precisely the
         * failure this tool was once fixed out of, so it is an error
         * here rather than a silent one three minutes into a boot. */
        fprintf(stderr,
                "leanfs-put: this image's leanfs magic is 0x%08X and this tool writes 0x%08X.\n"
                "            The on-disk format has moved - rebuild this tool against the\n"
                "            current kernel/fs/leanfs_format.h, and rebuild the image.\n",
                sb.magic, (unsigned)LEANFS_MAGIC);
        exit(1);
    } else {
        format_fresh();
    }
}

/* mkdir -p for the tree mode's destination, returning its inode. "/" is
 * the root and is not created; anything else is created if missing and
 * required to be a directory if not. */
static int resolve_dir(const char *path) {
    if (path[0] != '/') {
        die("leanfs paths are absolute - give a name like /gcc-15.1.0");
    }
    if (path[1] == '\0') {
        return ROOT_INODE;
    }
    char leaf[LEANFS_MAX_NAME + 1];
    int parent = resolve_parent(path, leaf);
    int idx = dir_lookup(&inodes[parent], leaf);
    if (idx < 0) {
        idx = alloc_inode(LEANFS_TYPE_DIR);
        dir_add(&inodes[parent], leaf, idx);
    } else if (inodes[idx].type != LEANFS_TYPE_DIR) {
        die("the destination path already exists and is not a directory");
    }
    return idx;
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s <disk-image> <local-file> <leanfs-path>\n", argv0);
    fprintf(stderr, "       %s -r <disk-image> <local-dir> <leanfs-dir>\n", argv0);
    fprintf(stderr, "       %s -s <disk-image> <target> <leanfs-path>\n\n", argv0);
    fprintf(stderr, "  Writes into disk-image's leanfs filesystem without booting anything.\n");
    fprintf(stderr, "  leanfs paths are absolute (\"/bin/yourprog\"); missing parent\n");
    fprintf(stderr, "  directories are created.\n\n");
    fprintf(stderr, "  -s makes leanfs-path a symbolic link to target. `target` is a\n");
    fprintf(stderr, "  string stored verbatim and is NOT resolved on the host - the link\n");
    fprintf(stderr, "  is being made for a filesystem this host cannot see. M89 added it\n");
    fprintf(stderr, "  for the multi-call binary, which needs one link per command name\n");
    fprintf(stderr, "  and has no host-side tree to copy them out of.\n\n");
    fprintf(stderr, "  -r copies a whole host directory tree, preserving symbolic links,\n");
    fprintf(stderr, "  hard links and modification times, and writes /.image-manifest\n");
    fprintf(stderr, "  describing what it put there.\n\n");
    fprintf(stderr, "  disk-image must already exist and be at least as large as `make all`\n");
    fprintf(stderr, "  produces (build/os-image.bin).\n");
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

    int recursive = (argc >= 2 && strcmp(argv[1], "-r") == 0);
    int symlink_mode = (argc >= 2 && strcmp(argv[1], "-s") == 0);
    int flagged = recursive || symlink_mode;
    if ((flagged && argc != 5) || (!flagged && argc != 4)) {
        usage(argv[0]);
        return 1;
    }

    const char *image_path  = argv[flagged ? 2 : 1];
    const char *local_path  = argv[flagged ? 3 : 2];
    const char *leanfs_path = argv[flagged ? 4 : 3];

    if (recursive) {
        struct stat st;
        if (lstat(local_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            die("local-dir is not a directory this tool can read");
        }
        image_open(image_path);

        int at = resolve_dir(leanfs_path);
        tree_root = leanfs_path;
        tree_mtime = (uint32_t)st.st_mtime;
        /* The tree's own root is a directory in the tree. put_tree counts
         * the ones it creates, which is every directory except this one -
         * and a walker starting here counts itself, so without this the
         * two sides disagree by exactly one. They did: the machine's first
         * run of this check reported "dirs 6/7" with every other field,
         * the content hash included, in agreement. */
        tally.dirs = 1;
        put_tree(local_path, at, (strcmp(leanfs_path, "/") == 0) ? "" : leanfs_path, 1);
        write_manifest(leanfs_path);
        save_all();
        fclose(img);

        printf("leanfs-put: %s -> %s:%s\n", local_path, image_path, leanfs_path);
        printf("            %llu dirs, %llu files (%llu bytes), %llu symlinks, "
               "%llu extra hard links, %llu skipped, %u deep\n",
               (unsigned long long)tally.dirs, (unsigned long long)tally.files,
               (unsigned long long)tally.bytes, (unsigned long long)tally.links,
               (unsigned long long)tally.hardlinks, (unsigned long long)tally.skipped,
               tally.deepest);
        return 0;
    }

    /* M89: a symbolic link whose target is a string rather than a path on
     * this host. The recursive mode above copies links it finds; this
     * makes one that has no host-side original - which is what a
     * multi-call binary needs, since its hundred and fifty names exist
     * only on the machine being built. */
    if (symlink_mode) {
        size_t tlen = strlen(local_path);
        if (tlen == 0 || tlen >= LEANFS_MAX_PATH) {
            die("the link target is empty or longer than a path");
        }
        image_open(image_path);
        char leaf[LEANFS_MAX_NAME + 1];
        int parent = resolve_parent(leanfs_path, leaf);
        int idx = dir_lookup(&inodes[parent], leaf);
        if (idx >= 0 && inodes[idx].type == LEANFS_TYPE_LINK) {
            free_inode_blocks(&inodes[idx]); /* re-pointing an existing link */
        } else if (idx >= 0) {
            if (inodes[idx].type == LEANFS_TYPE_DIR) {
                die("that path already names a directory");
            }
            /* A regular file being replaced by a link. This is the
             * multi-call install overwriting one of this project's own
             * programs, which M89 decided in favour of the ported name -
             * see the Makefile's `toybox` target. The old inode's blocks
             * are released and the directory record is repointed, so the
             * name never stops resolving. */
            free_inode_blocks(&inodes[idx]);
            memset(&inodes[idx], 0, sizeof(inodes[idx]));
            idx = alloc_inode(LEANFS_TYPE_LINK);
            dir_repoint(&inodes[parent], leaf, idx);
        } else {
            idx = alloc_inode(LEANFS_TYPE_LINK);
            dir_add(&inodes[parent], leaf, idx);
        }
        inode_write_all(&inodes[idx], (const uint8_t *)local_path, tlen,
                        (uint32_t)time(NULL));
        save_all();
        fclose(img);
        printf("leanfs-put: %s -> %s (symlink)\n", leanfs_path, local_path);
        return 0;
    }

    struct stat st;
    if (lstat(local_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        die("local-file is not a regular file this tool can read");
    }
    if ((uint64_t)st.st_size > (uint64_t)LEANFS_MAX_FILE_SIZE) {
        die("local-file exceeds LEANFS_MAX_FILE_SIZE");
    }
    FILE *src = fopen(local_path, "rb");
    if (!src) {
        die("could not open local-file");
    }

    image_open(image_path);

    char leaf[LEANFS_MAX_NAME + 1];
    int parent = resolve_parent(leanfs_path, leaf);

    int idx = dir_lookup(&inodes[parent], leaf);
    if (idx >= 0 && inodes[idx].type == LEANFS_TYPE_LINK) {
        /* M89: a symbolic link being replaced by a file. The mirror of
         * the -s path's file-to-link case, and needed for the same
         * reason: `make preseed` after `make toybox` writes a program
         * over a name a previous install had turned into a link, and a
         * tool that could only go one way would leave an image nothing
         * could put back. */
        free_inode_blocks(&inodes[idx]);
        memset(&inodes[idx], 0, sizeof(inodes[idx]));
        idx = alloc_inode(LEANFS_TYPE_FILE);
        dir_repoint(&inodes[parent], leaf, idx);
    } else if (idx >= 0) {
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

    inode_write_stream(&inodes[idx], src, (uint64_t)st.st_size,
                       (uint32_t)st.st_mtime, local_path, NULL);
    save_all();

    fclose(img);
    fclose(src);
    printf("leanfs-put: wrote %lld bytes to %s as %s\n",
           (long long)st.st_size, image_path, leanfs_path);
    return 0;
}
