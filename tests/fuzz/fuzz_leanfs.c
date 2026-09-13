#include "file_system/leanfs.h"

#include "file_system/leanfs_format.h"

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

    const uint32_t sectors = (uint32_t)((LEANFS_START_BLOCK + 1 +
                                         (LEANFS_MAX_INODES * 128 / LEANFS_BLOCK_SIZE) +
                                         (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE) +
                                         64) * LEANFS_SECTORS_PER_BLOCK);
    klog_capture_reset();
    fake_blk_reset(sectors);

    size_t n = size;
    size_t room = (size_t)(fake_blk_sector_count() - LEANFS_START_LBA) * 512;
    if (n > room) {
        n = room;
    }
    uint8_t *sb = fake_blk_sector(LEANFS_START_LBA);
    memcpy(sb, data, n);

    if ((data[0] & 1) == 0) {
        const uint32_t magic = LEANFS_MAGIC;
        memcpy(sb, &magic, sizeof(magic));
    }

    leanfs_init();

    char buf[128];
    leanfs_is_dir("/");
    leanfs_exists("/bin");
    leanfs_read("/bin/hello", buf, sizeof(buf));
    uint32_t cookie = 0;
    leanfs_dir_entry_t ent;
    for (int i = 0; i < 64 && leanfs_readdir("/", &cookie, &ent) == 1; i++) {
    }
    leanfs_write("/afterwards", "ok", 2);

    fake_blk_free();
    return 0;
}
