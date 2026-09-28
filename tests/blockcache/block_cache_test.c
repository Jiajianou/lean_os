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

void fake_backend_present(int nvme, int usb);
void fake_backend_label_usb(int labelled);
void fake_backend_label_primary(int labelled);

/* M186. On a machine booted from a stick there is more than one disk and only
   one of them is this OS's. Probe order alone picks the internal drive, which
   on a laptop belongs to somebody else. */
static void choose_with(int nvme, int usb, int nvme_labelled, int usb_labelled) {
    fake_backend_reset(DISK_SECTORS);
    fill_disk();
    fake_backend_present(nvme, usb);
    fake_backend_label_primary(nvme_labelled);
    fake_backend_label_usb(usb_labelled);
    block_device_init();
}

/* These tests leave the block layer pointed at whichever fake they were
   about; every other test in this file assumes the big primary disk, and the
   runner does not promise an order. */
static void restore_default_disk(void) {
    fake_backend_present(0, 0);
    fake_backend_reset(DISK_SECTORS);
    fill_disk();
    block_device_init();
}

TEST(block_selection, the_stick_is_chosen_over_an_internal_disk_that_is_not_ours) {
    choose_with(1, 1, 0, 1);
    CHECK(strcmp(block_device_backend_name(), "usb-storage") == 0);
    restore_default_disk();
}

TEST(block_selection, an_internal_disk_that_is_ours_is_chosen_over_a_stick_that_is_not) {
    choose_with(1, 1, 1, 0);
    CHECK(strcmp(block_device_backend_name(), "nvme") == 0);
    restore_default_disk();
}

TEST(block_selection, with_no_labelled_disk_at_all_the_first_probed_is_kept) {
    choose_with(1, 1, 0, 0);
    CHECK(strcmp(block_device_backend_name(), "nvme") == 0);
    restore_default_disk();
}

TEST(block_selection, a_stick_on_its_own_is_chosen_whether_or_not_it_is_labelled) {
    choose_with(0, 1, 0, 1);
    CHECK(strcmp(block_device_backend_name(), "usb-storage") == 0);
    restore_default_disk();
}

TEST(block_selection, probe_order_still_decides_when_both_disks_are_ours) {
    choose_with(1, 1, 1, 1);
    CHECK(strcmp(block_device_backend_name(), "nvme") == 0);
    restore_default_disk();
}

/* M191. Frames are 4 KiB, so a count of frames is a count of bytes / 4096. */
#define GIB_OF_FRAMES(n) ((uint64_t)(n) * 1024 * 1024 * 1024 / 4096)

TEST(block_cache_size, a_laptop_holds_a_whole_browser_executable) {
    uint32_t lines = block_device_cache_lines_for(GIB_OF_FRAMES(16));
    CHECK((uint64_t)lines * 4096 >= 512ull * 1024 * 1024);
    CHECK_EQ(lines, BLOCK_DEVICE_MAX_CACHE_LINES);
    CHECK_EQ(block_device_cache_lines_for(GIB_OF_FRAMES(64)), BLOCK_DEVICE_MAX_CACHE_LINES);
}

TEST(block_cache_size, a_small_machine_keeps_what_it_had_before) {
    CHECK_EQ(block_device_cache_lines_for(GIB_OF_FRAMES(4)), (uint32_t)(GIB_OF_FRAMES(4) / 64));
    CHECK_EQ(block_device_cache_lines_for(32768), 2048u);
    CHECK_EQ(block_device_cache_lines_for(16384), 1024u);
    CHECK_EQ(block_device_cache_lines_for(0), 64u);
}

TEST(block_cache_size, more_memory_never_means_a_smaller_cache) {
    uint32_t previous = 0;
    for (uint64_t frames = 0; frames <= GIB_OF_FRAMES(32); frames += GIB_OF_FRAMES(1) / 8) {
        uint32_t lines = block_device_cache_lines_for(frames);
        CHECK(lines >= previous);
        previous = lines;
    }
}

TEST(block_cache, readahead_fetches_its_whole_window_in_one_request) {
    setup();
    block_device_set_readahead(16);
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    CHECK_EQ(block_device_read(200 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_read(0, SECTORS_PER_LINE, line), 0);
    fake_backend_reset_counters();
    CHECK_EQ(block_device_read(SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    expect_sector(line, SECTORS_PER_LINE);
    CHECK_EQ((int)fake_backend_read_calls(), 2);

    fake_backend_reset_counters();
    for (uint32_t k = 2; k < 2 + 16; k++) {
        CHECK_EQ(block_device_read(k * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
        expect_sector(line, k * SECTORS_PER_LINE);
        expect_sector(line + 7 * BLOCK_DEVICE_SECTOR_SIZE, k * SECTORS_PER_LINE + 7);
    }
    CHECK_EQ((int)fake_backend_read_calls(), 0);
    block_device_set_readahead(0);
}

/* M194: the journal, over the same fake disk. The filesystem is lines
   0..255 and the journal the 128 blocks after it; a crash is losing
   everything in memory - the transaction and the cache - and attaching
   again, which is what a power cut and a boot are. */
#include "file_system/leanfs_format.h"

#define J_FS_LBA 0u
#define J_FS_SECTORS (256u * SECTORS_PER_LINE)
#define J_LBA (256u * SECTORS_PER_LINE)
#define J_BLOCKS 128u

static int journal_attach(void) {
    return block_device_journal_attach(J_FS_LBA, J_FS_SECTORS, J_LBA, J_BLOCKS);
}

static void journal_setup(void) {
    setup();
    block_device_journal_forget();
    memset(fake_backend_sector(J_LBA), 0, BLOCK_DEVICE_SECTOR_SIZE);
    CHECK_EQ(journal_attach(), 0);
    fake_backend_reset_counters();
}

static void journal_crash(void) {
    block_device_journal_forget();
    block_device_init();
}

static void journal_teardown(void) {
    block_device_journal_forget();
}

static void fill_line(uint8_t *line, uint8_t value) {
    memset(line, value, SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE);
}

static int line_on_disk_is(uint32_t line_no, uint8_t value) {
    for (uint32_t k = 0; k < SECTORS_PER_LINE; k++) {
        const uint8_t *sector = fake_backend_sector(line_no * SECTORS_PER_LINE + k);
        for (uint32_t i = 0; i < BLOCK_DEVICE_SECTOR_SIZE; i++) {
            if (sector[i] != value) {
                return 0;
            }
        }
    }
    return 1;
}

TEST(journal, a_write_reaches_the_disk_only_through_a_commit) {
    journal_setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    fill_line(line, 0xA1);
    CHECK_EQ(block_device_write(10 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_flush(), 0);
    CHECK_EQ((int)fake_backend_write_calls(), 0);

    uint8_t back[sizeof(line)];
    CHECK_EQ(block_device_read(10 * SECTORS_PER_LINE, SECTORS_PER_LINE, back), 0);
    CHECK_EQ(back[0], 0xA1);
    CHECK_EQ(back[sizeof(back) - 1], 0xA1);

    CHECK_EQ(block_device_commit(), 0);
    CHECK(fake_backend_write_calls() > 0);
    CHECK(!line_on_disk_is(10, 0xA1));
    CHECK_EQ(block_device_checkpoint(), 0);
    CHECK(line_on_disk_is(10, 0xA1));
    journal_teardown();
}

TEST(journal, a_committed_transaction_survives_the_power_going) {
    journal_setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    fill_line(line, 0xB2);
    CHECK_EQ(block_device_write(11 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_write(50 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_commit(), 0);
    journal_crash();
    CHECK(!line_on_disk_is(11, 0xB2));
    CHECK_EQ(journal_attach(), 1);
    CHECK(line_on_disk_is(11, 0xB2));
    CHECK(line_on_disk_is(50, 0xB2));
    CHECK_EQ(journal_attach(), 0);
    journal_teardown();
}

TEST(journal, an_uncommitted_transaction_is_lost_whole_and_the_one_before_it_is_not) {
    journal_setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    fill_line(line, 0xC3);
    CHECK_EQ(block_device_write(12 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_commit(), 0);
    fill_line(line, 0xC4);
    CHECK_EQ(block_device_write(13 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_write(12 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    journal_crash();
    CHECK_EQ(journal_attach(), 1);
    CHECK(line_on_disk_is(12, 0xC3));
    CHECK(!line_on_disk_is(13, 0xC4));
    CHECK(!line_on_disk_is(12, 0xC4));
    journal_teardown();
}

TEST(journal, a_torn_transaction_is_not_replayed) {
    journal_setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    fill_line(line, 0xD5);
    CHECK_EQ(block_device_write(14 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_write(15 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_commit(), 0);
    /* Block 1 is the descriptor, 2 and 3 the two lines: one byte of the
       second line never made it. */
    fake_backend_sector(J_LBA + 3 * SECTORS_PER_LINE + 5)[17] ^= 0x40;
    journal_crash();
    CHECK_EQ(journal_attach(), 0);
    CHECK(!line_on_disk_is(14, 0xD5));
    CHECK(!line_on_disk_is(15, 0xD5));
    journal_teardown();
}

TEST(journal, the_later_of_two_transactions_is_what_a_replay_leaves) {
    journal_setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    fill_line(line, 0x11);
    CHECK_EQ(block_device_write(16 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_commit(), 0);
    fill_line(line, 0x22);
    CHECK_EQ(block_device_write(16 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_commit(), 0);
    journal_crash();
    CHECK_EQ(journal_attach(), 2);
    CHECK(line_on_disk_is(16, 0x22));
    journal_teardown();
}

TEST(journal, commits_past_the_end_of_the_journal_checkpoint_and_lose_nothing) {
    journal_setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    for (uint32_t round = 0; round < 40; round++) {
        for (uint32_t k = 0; k < 5; k++) {
            fill_line(line, (uint8_t)(round + k));
            CHECK_EQ(block_device_write((100 + k) * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
        }
        CHECK_EQ(block_device_commit(), 0);
    }
    block_device_statistics_t stats;
    block_device_statistics(&stats);
    CHECK(stats.journal_checkpoints > 0);
    journal_crash();
    CHECK(journal_attach() >= 0);
    for (uint32_t k = 0; k < 5; k++) {
        CHECK(line_on_disk_is(100 + k, (uint8_t)(39 + k)));
    }
    journal_teardown();
}

TEST(journal, a_hundred_updates_to_the_same_blocks_are_one_commit_of_them) {
    journal_setup();
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    for (uint32_t op = 0; op < 100; op++) {
        for (uint32_t k = 0; k < 3; k++) {
            fill_line(line, (uint8_t)op);
            CHECK_EQ(block_device_write((20 + k * 7) * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
        }
    }
    CHECK_EQ((int)fake_backend_write_calls(), 0);
    CHECK_EQ(block_device_commit(), 0);
    /* The header, one sequential run of descriptor and three blocks, and the
       commit block - where M104's barriers issued three hundred. */
    CHECK((int)fake_backend_write_calls() <= 3);
    journal_teardown();
}

TEST(journal, a_disk_without_a_journal_is_not_written_by_attaching) {
    setup();
    block_device_journal_forget();
    memset(fake_backend_sector(J_LBA), 0, BLOCK_DEVICE_SECTOR_SIZE);
    fake_backend_reset_counters();
    CHECK_EQ(journal_attach(), 0);
    CHECK_EQ((int)fake_backend_write_calls(), 0);
    journal_teardown();
}

TEST(journal, one_sector_of_a_block_is_journaled_with_the_rest_of_its_block) {
    journal_setup();
    uint8_t sector[BLOCK_DEVICE_SECTOR_SIZE];
    memset(sector, 0xE7, sizeof(sector));
    CHECK_EQ(block_device_write(30 * SECTORS_PER_LINE + 3, 1, sector), 0);
    CHECK_EQ(block_device_commit(), 0);
    journal_crash();
    CHECK_EQ(journal_attach(), 1);
    CHECK_EQ(fake_backend_sector(30 * SECTORS_PER_LINE + 3)[0], 0xE7);
    CHECK_EQ(fake_backend_sector(30 * SECTORS_PER_LINE + 2)[0], (uint8_t)((30 * SECTORS_PER_LINE + 2) & 0xff));
    CHECK_EQ(fake_backend_sector(30 * SECTORS_PER_LINE + 4)[0], (uint8_t)((30 * SECTORS_PER_LINE + 4) & 0xff));
    journal_teardown();
}

TEST(journal, a_write_outside_the_filesystem_is_not_journaled) {
    journal_setup();
    block_device_statistics_t before, after;
    block_device_statistics(&before);
    uint8_t line[SECTORS_PER_LINE * BLOCK_DEVICE_SECTOR_SIZE];
    fill_line(line, 0x5A);
    CHECK_EQ(block_device_write(450 * SECTORS_PER_LINE, SECTORS_PER_LINE, line), 0);
    CHECK_EQ(block_device_commit(), 0);
    CHECK_EQ(block_device_flush(), 0);
    CHECK(line_on_disk_is(450, 0x5A));
    block_device_statistics(&after);
    CHECK_EQ((int)(after.journal_commits - before.journal_commits), 0);
    journal_teardown();
}
