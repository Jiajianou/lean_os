/* kernel/fs/leanfs_format.h - Q3
 *
 * The on-disk format, in one place, for everyone who writes this disk.
 *
 * ---- Why this file exists --------------------------------------------
 *
 * There have always been two programs that write a leanfs image: the
 * kernel's own driver, and tools/leanfs-put.c, which puts a file straight
 * into an image host-side without booting anything. Until Q3 the second
 * one carried its own hand-copied declarations of the superblock, the
 * inode and the directory record, under a comment that said:
 *
 *     The structs below still have to match kernel/fs/leanfs.c byte for
 *     byte.
 *
 * That sentence is a correct description of a real constraint and it was
 * enforced by nothing at all. Two independent definitions of an on-disk
 * format, kept in agreement by a comment, is a data-corruption bug with a
 * long fuse: the day they disagree, the tool writes an image the kernel
 * misreads, and the symptom is a filesystem rather than a compile error.
 *
 * The definitions lived in leanfs.c rather than leanfs.h, which is what
 * made the copy necessary - a host tool cannot include a .c file. So they
 * move here, this header is included by both, and the duplication is
 * gone. Everything below is moved verbatim from leanfs.c; the comments
 * are the originals and are worth reading where they are.
 *
 * The _Static_asserts travel with the structs on purpose. They are what
 * turns "an inode must be 128 bytes so four fit a sector exactly" from a
 * sentence into a build failure, and they now fire for the host tool too.
 */
#pragma once

#include "leanfs.h"

#include <stddef.h>
#include <stdint.h>

/* ---- M93 (second attempt): /.image-manifest ---------------------------
 *
 * A host tool writes a tree onto this disk (tools/leanfs-put.c -r) and the
 * machine has to be able to say whether it agrees. The manifest is how:
 * a small text file at /.image-manifest naming the tree and stating, in
 * numbers, what the host put in it. kernel.c walks the tree at boot and
 * compares. See the self-test there for what each number proves.
 *
 * The hash below is why this lives in the shared format header rather
 * than in either program. It is not part of the on-disk *format* - it is
 * part of the contract between the writer and the reader of one file -
 * but it has exactly the property that made Q3 delete the tool's private
 * copy of the structs: two implementations of it, kept in agreement by a
 * comment, would disagree one day and the symptom would be a self-test
 * failing on a correct image.
 *
 * FNV-1a, and deliberately not a CRC: no table, no dependency, and the
 * only thing being defended against is a file that did not survive the
 * trip. The aggregate over a tree is a wrapping SUM of one hash per name,
 * which makes it independent of the order the two sides walk in - the
 * host sorts its entries and the machine reads directory records in the
 * order they were written, and requiring those to match would be
 * requiring something neither side promises.
 */
#define LEANFS_FNV1A_INIT 0x811C9DC5u

static inline uint32_t leanfs_fnv1a(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

#define LEANFS_MAGIC     0x3553464Cu /* "LFS5". M93: bumped from M81's 0x3453464C, and for the same reason M81 bumped it - every region moved. A block is 4096 bytes rather than 512, the inode table went from 8192 entries to 131072, and the data region from 32 MiB to 2 GiB. An old disk read with this layout would resolve garbage block numbers, so it is reformatted rather than misread. The version field below still cannot help: the geometry is what changed, and there is nowhere on a 35 MiB disk to stand while relocating it into a 2 GiB one. Previously: "LFS4". M81: bumped from M59's 0x3353464C (itself M53's 0x3253464C, itself M12's 0x3153464C). Every region moved: the inode table grew from 32 sectors to 2048, which pushes the bitmap and the whole data region down the disk, and directory records stopped being fixed-size. An old disk read with this layout would resolve garbage block numbers, so it is reformatted rather than misread - see leanfs_init, where M81 also adds the version field that makes a *future* bump able to do better than that. */
#define LEANFS_VERSION   5u          /* M93: 4 -> 5, alongside the magic. M81: see sb.version. Bumped only when the on-disk meaning changes; the magic is bumped only when the geometry does. */
/* M92: moved to leanfs.h - kernel.c's block-layer self-test needs an
 * LBA that is really this filesystem's, and a second copy of 8192 would
 * be the drift the Makefile comment already warns about. */

#define LEANFS_TYPE_FREE 0
#define LEANFS_TYPE_FILE 1
#define LEANFS_TYPE_DIR  2
/* M87: a symbolic link. Its target is stored in its data blocks exactly
 * as a regular file's contents are, with `size` the target's length -
 * which is why this needed no format change at all beyond the type
 * value. An old disk has no inode of type 3, so a disk written before
 * this milestone is still valid and the magic did not have to move. */
#define LEANFS_TYPE_LINK 3

#define ROOT_INODE 0

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_table_block;
    uint32_t inode_table_blocks;
    uint32_t bitmap_block;
    uint32_t bitmap_blocks_field;
    uint32_t data_block;
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
    /* M93: how many directory records name this inode.
     *
     * M81 reserved these 44 bytes for exactly this - "M87 is scheduled to
     * add a link count and a symlink target; this is the room for them,
     * reserved because that is scheduled work and not because a field
     * might one day be nice." M87 added symlinks and needed no field for
     * them (a link's target lives in its data blocks). This is the other
     * half, arriving one milestone late.
     *
     * 1 for an ordinary file, more once something calls leanfs_link.
     * A zero here on a disk written before M93 is impossible: the magic
     * changed with it, so every inode this build ever reads was written
     * by this build. That is the only reason this can be a plain field
     * rather than a field with a "0 means 1" rule, and it is worth
     * stating because the next format change may not get to say it. */
    uint32_t nlink;
    uint8_t  reserved[40];
} leanfs_inode_t;

_Static_assert(sizeof(leanfs_inode_t) == 128, "an inode must be 128 bytes so four fit a sector exactly");
_Static_assert(LEANFS_BLOCK_SIZE % sizeof(leanfs_inode_t) == 0, "an inode must not straddle a sector");

#define INODE_TABLE_BLOCKS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_BLOCKS       (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE)

/* M81: the record header is variable-length now, so the old "16 fit a
 * block exactly" assertion is gone - the no-straddling guarantee it
 * protected is enforced instead by dir_block_place and dir_block_walk,
 * which never let a rec_len run past the end of its block. What is still
 * a compile-time claim is the header's own size, because
 * LEANFS_DIRENT_NEED in leanfs.h computes with the number rather than
 * the type. */
_Static_assert(sizeof(leanfs_dirent_t) == LEANFS_DIRENT_HDR, "leanfs_dirent_t header must stay 8 bytes");
_Static_assert(LEANFS_DIRENT_NEED(LEANFS_MAX_NAME) <= LEANFS_BLOCK_SIZE, "the longest name must still fit in one block");
