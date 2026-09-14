#include "check.h"
#include "drivers/block_device.h"

#include <stdint.h>
#include <string.h>

#define SECTORS_PER_LINE 8
#define DISK_SECTORS     4096

void fake_backend_reset(uint32_t sectors);
void fake_backend_reset_counters(void);
uint64_t fake_backend_read_calls(void);
uint64_t fake_backend_write_calls(void);
uint8_t *fake_backend_sector(uint32_t lba);

static int initialised;

static void fill_disk(void) {
    for (uint32_t lba = 0; lba < DISK_SECTORS; lba++) {
        memset(fake_backend_sector(lba), (int)(lba & 0xff), BLOCK_DEVICE_SECTOR_SIZE);
    }
}

static void setup(void) {
    fake_backend_reset(DISK_SECTORS);
    fill_disk();
    if (!initialised) {
        block_device_init();
        initialised = 1;
    }
    block_device_cache_drop();
    fake_backend_reset_counters();
}

static void expect_sector(const uint8_t *buffer, uint32_t lba) {
    for (uint32_t i = 0; i < BLOCK_DEVICE_SECTOR_SIZE; i++) {
        CHECK_EQ(buffer[i], (uint8_t)(lba & 0xff));
    }
}

TEST(block_cache, a_sub_line_read_populates_the_cache) {
    setup();
    uint8_t one[BLOCK_DEVICE_SECTOR_SIZE];
    CHECK_EQ(block_device_read(3, 1, one), 0);
    expect_sector(one, 3);
    uint64_t after_first = fake_backend_read_calls();
    CHECK(after_first > 0);

    memset(one, 0, sizeof(one));
    CHECK_EQ(block_device_read(3, 1, one), 0);
    expect_sector(one, 3);
    CHECK_EQ((int)(fake_backend_read_calls() - after_first), 0);
}

TEST(block_cache, an_unaligned_read_across_two_lines_is_cached) {
    setup();
    uint8_t six[6 * BLOCK_DEVICE_SECTOR_SIZE];
    CHECK_EQ(block_device_read(5, 6, six), 0);
    for (uint32_t i = 0; i < 6; i++) {
        expect_sector(six + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE, 5 + i);
    }
    uint64_t after_first = fake_backend_read_calls();

    memset(six, 0, sizeof(six));
    CHECK_EQ(block_device_read(5, 6, six), 0);
    for (uint32_t i = 0; i < 6; i++) {
        expect_sector(six + (uint64_t)i * BLOCK_DEVICE_SECTOR_SIZE, 5 + i);
    }
    CHECK_EQ((int)(fake_backend_read_calls() - after_first), 0);
}

TEST(block_cache, a_partial_line_write_is_one_device_call) {
    setup();
    uint8_t seven[7 * BLOCK_DEVICE_SECTOR_SIZE];
    memset(seven, 0xa5, sizeof(seven));
    CHECK_EQ(block_device_write(1, 7, seven), 0);
    CHECK_EQ((int)fake_backend_write_calls(), 1);

    for (uint32_t i = 0; i < 7; i++) {
        const uint8_t *on_disk = fake_backend_sector(1 + i);
        CHECK_EQ(on_disk[0], 0xa5);
    }
}

TEST(block_cache, a_dirty_line_is_not_clobbered_by_an_overlapping_read) {
    setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    memset(line, 0x5c, sizeof(line));
    CHECK_EQ(block_device_write(SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ((int)fake_backend_write_calls(), 0);

    uint8_t across[4 * BLOCK_DEVICE_SECTOR_SIZE];
    CHECK_EQ(block_device_read(6, 4, across), 0);
    expect_sector(across, 6);
    expect_sector(across + BLOCK_DEVICE_SECTOR_SIZE, 7);
    CHECK_EQ(across[2 * BLOCK_DEVICE_SECTOR_SIZE], 0x5c);
    CHECK_EQ(across[3 * BLOCK_DEVICE_SECTOR_SIZE], 0x5c);

    CHECK_EQ(block_device_flush(), 0);
    CHECK_EQ(fake_backend_sector(SECTORS_PER_LINE)[0], 0x5c);
    CHECK_EQ(fake_backend_sector(SECTORS_PER_LINE + 7)[0], 0x5c);
}

TEST(block_cache, a_whole_line_write_stays_in_the_cache_until_flushed) {
    setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    memset(line, 0x3b, sizeof(line));
    CHECK_EQ(block_device_write(16, SECTORS_PER_LINE, line), 0);
    CHECK_EQ((int)fake_backend_write_calls(), 0);
    CHECK_EQ(fake_backend_sector(16)[0], 16);

    CHECK_EQ(block_device_flush(), 0);
    CHECK_EQ(fake_backend_sector(16)[0], 0x3b);
}

TEST(block_cache, a_flush_writes_contiguous_dirty_lines_as_one_call) {
    setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    memset(line, 0x77, sizeof(line));
    for (uint32_t k = 0; k < 6; k++) {
        CHECK_EQ(block_device_write((24 + k) * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    }
    CHECK_EQ((int)fake_backend_write_calls(), 0);

    CHECK_EQ(block_device_flush(), 0);
    CHECK_EQ((int)fake_backend_write_calls(), 1);
    for (uint32_t k = 0; k < 6; k++) {
        CHECK_EQ(fake_backend_sector((24 + k) * SECTORS_PER_LINE)[0], 0x77);
        CHECK_EQ(fake_backend_sector((24 + k) * SECTORS_PER_LINE + 7)[0], 0x77);
    }
}

TEST(block_cache, a_flush_of_scattered_lines_writes_each_run_once) {
    setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    memset(line, 0x11, sizeof(line));
    CHECK_EQ(block_device_write(32 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_write(33 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_write(90 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);

    CHECK_EQ(block_device_flush(), 0);
    CHECK_EQ((int)fake_backend_write_calls(), 2);
}
