/* tests/fuzz/fuzz_leanfs.c - Q4
 *
 * A filesystem image made of arbitrary bytes, mounted.
 *
 * leanfs_init reads a superblock written by something else and sizes
 * every subsequent buffer from the fields in it. leanfs.c's own comment
 * is explicit that this is the danger - "any mismatch means a superblock
 * this build cannot safely interpret ... reformatting is what keeps a
 * corrupted field from turning into an out-of-bounds write" - and the
 * unit tests check the specific corruptions a person thought of. This
 * checks the rest.
 *
 * The mount is followed by a few ordinary operations, because a
 * superblock that survives being read can still produce a directory walk
 * that does not. */
#include "fs/leanfs.h"

#include "fs/leanfs_format.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void fake_blk_reset(uint32_t sectors);
void fake_blk_free(void);
uint8_t *fake_blk_sector(uint32_t lba);
uint32_t fake_blk_sector_count(void);
void klog_capture_reset(void);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 64) {
        return 0;
    }

    /* Only as much disk as the metadata regions need. The data region is
     * two gigabytes and allocating it per input would make this fuzzer
     * far too slow to find anything; the blocks that matter for a mount
     * are the superblock, the inode table and the bitmap, and a short
     * disk means an access past them is a loud panic from the fake rather
     * than a silent success. That is the behaviour worth having here. */
    const uint32_t sectors = (uint32_t)((LEANFS_START_BLOCK + 1 +
                                         (LEANFS_MAX_INODES * 128 / LEANFS_BLOCK_SIZE) +
                                         (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE) +
                                         64) * LEANFS_SECTORS_PER_BLOCK);
    /* Deliberately no fake_pmm_reset(). leanfs holds its 16 MiB inode
     * table for the life of the process (inodes_alloc), so freeing the
     * frames under it between inputs leaves that pointer dangling - ASan
     * reports it on the fourth input, which is what caught the same
     * mistake in the unit-test fixture. */
    klog_capture_reset();
    fake_blk_reset(sectors);

    /* The input is the superblock, and as much of the inode table as it
     * reaches. Those are the bytes leanfs believes. */
    size_t n = size;
    size_t room = (size_t)(fake_blk_sector_count() - LEANFS_START_LBA) * 512;
    if (n > room) {
        n = room;
    }
    uint8_t *sb = fake_blk_sector(LEANFS_START_LBA);
    memcpy(sb, data, n);

    /* Half the inputs get a valid magic stamped over whatever was there.
     *
     * Without this the fuzzer is almost useless: a random superblock
     * essentially never has the right magic, so essentially every input
     * takes the "reformat and start again" branch - which is safe by
     * construction and is not the branch worth exploring. The dangerous
     * path is the one where the magic *matches* and leanfs therefore
     * believes the geometry fields that follow, because those fields size
     * the buffers every later read indexes into. This is how the fuzzer
     * gets there.
     *
     * The other half is left alone so the reformat path keeps its
     * coverage too. */
    if ((data[0] & 1) == 0) {
        const uint32_t magic = LEANFS_MAGIC;
        memcpy(sb, &magic, sizeof(magic));
    }

    leanfs_init();

    /* A mount that survived still has to survive being used. */
    char buf[128];
    leanfs_is_dir("/");
    leanfs_exists("/bin");
    leanfs_read("/bin/hello", buf, sizeof(buf));
    uint32_t cookie = 0;
    leanfs_dir_entry_t ent;
    for (int i = 0; i < 64 && leanfs_readdir("/", &cookie, &ent) == 1; i++) {
        /* walking is the point */
    }
    leanfs_write("/afterwards", "ok", 2);

    fake_blk_free();
    return 0;
}
