#include "file_system/leanfs.h"

#include "file_system/leanfs_format.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void fake_block_device_reset(uint32_t sectors);
void fake_block_device_free(void);
uint8_t *fake_block_device_sector(uint32_t lba);
uint32_t fake_block_device_sector_count(void);
void kernel_log_capture_reset(void);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 64) {
        return 0;
    }

    const uint32_t sectors = (uint32_t)((LEANFS_START_BLOCK + 1 +
                                         (LEANFS_MAX_INODES * 128 / LEANFS_BLOCK_SIZE) +
                                         (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE) +
                                         64) * LEANFS_SECTORS_PER_BLOCK);
    kernel_log_capture_reset();
    fake_block_device_reset(sectors);

    size_t n = size;
    size_t room = (size_t)(fake_block_device_sector_count() - LEANFS_START_LBA) * 512;
    if (n > room) {
        n = room;
    }
    uint8_t *sb = fake_block_device_sector(LEANFS_START_LBA);
    memcpy(sb, data, n);

    if ((data[0] & 1) == 0) {
        const uint32_t magic = LEANFS_MAGIC;
        memcpy(sb, &magic, sizeof(magic));
    }

    leanfs_init();

    char buffer[128];
    leanfs_is_directory("/");
    leanfs_exists("/bin");
    leanfs_read("/bin/hello", buffer, sizeof(buffer));
    uint32_t cookie = 0;
    leanfs_directory_entry_t entry;
    for (int i = 0; i < 64 && leanfs_readdir("/", &cookie, &entry) == 1; i++) {
    }
    leanfs_write("/afterwards", "ok", 2);

    fake_block_device_free();
    return 0;
}
