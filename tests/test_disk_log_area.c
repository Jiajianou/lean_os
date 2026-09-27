#include "check.h"

#include "boot/boot_options.h"
#include "drivers/disk_log_area.h"

#include <string.h>

#define FAKE_DISK_SECTORS 512
#define AREA_LBA 100

typedef struct {
    uint8_t sectors[FAKE_DISK_SECTORS][512];
    int writes;
    int fail_writes;
    int touched_outside;
    uint32_t area_sectors;
} fake_disk_t;

static fake_disk_t disk;
static disk_log_area_t area;

static int in_area(uint32_t lba, uint32_t count) {
    return lba >= AREA_LBA && lba + count <= AREA_LBA + disk.area_sectors;
}

static int fake_read(void *context, uint32_t lba, uint32_t count, void *buffer) {
    fake_disk_t *d = context;
    if (!in_area(lba, count)) {
        d->touched_outside = 1;
        return -1;
    }
    memcpy(buffer, d->sectors[lba], (size_t)count * 512);
    return 0;
}

static int fake_write(void *context, uint32_t lba, uint32_t count, void *buffer) {
    fake_disk_t *d = context;
    if (!in_area(lba, count)) {
        d->touched_outside = 1;
        return -1;
    }
    if (d->fail_writes) {
        return -1;
    }
    memcpy(d->sectors[lba], buffer, (size_t)count * 512);
    d->writes++;
    return 0;
}

static void format_area(uint32_t sectors) {
    memset(&disk, 0x5A, sizeof(disk.sectors));
    disk.writes = 0;
    disk.fail_writes = 0;
    disk.touched_outside = 0;
    disk.area_sectors = sectors;
    for (uint32_t i = 0; i < sectors; i++) {
        memset(disk.sectors[AREA_LBA + i], '\n', 512);
    }
    const char *header = "LEAN_OS LOG AREA 1\nnext=0\nwrapped=0\nboots=0\n";
    memcpy(disk.sectors[AREA_LBA], header, strlen(header));
}

static int open_area(void) {
    return disk_log_area_open(&area, AREA_LBA, disk.area_sectors, fake_read, fake_write, &disk);
}

static const uint8_t *data_at(uint32_t offset) {
    return &disk.sectors[AREA_LBA + 1 + offset / 512][offset % 512];
}

static void copy_out(uint32_t offset, char *out, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        out[i] = (char)*data_at(offset + i);
    }
}

static void pattern(char *out, uint32_t length, uint32_t seed) {
    for (uint32_t i = 0; i < length; i++) {
        out[i] = (char)('a' + (i + seed) % 26);
    }
}

TEST(disk_log_area, a_sector_without_the_header_is_refused_and_nothing_is_written) {
    format_area(16);
    memset(disk.sectors[AREA_LBA], 0, 512);
    CHECK_EQ(open_area(), DISK_LOG_AREA_NOT_AN_AREA);
    CHECK_EQ(disk.writes, 0);
}

TEST(disk_log_area, a_header_with_a_field_missing_is_not_an_area) {
    format_area(16);
    memset(disk.sectors[AREA_LBA], '\n', 512);
    const char *header = "LEAN_OS LOG AREA 1\nnext=0\nboots=0\n";
    memcpy(disk.sectors[AREA_LBA], header, strlen(header));
    CHECK_EQ(open_area(), DISK_LOG_AREA_NOT_AN_AREA);
}

TEST(disk_log_area, the_first_open_counts_one_boot_and_starts_at_the_beginning) {
    format_area(16);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    CHECK_EQ(area.boots, 1u);
    CHECK_EQ(area.next, 0u);
    CHECK_EQ(area.data_sectors, 15u);
}

TEST(disk_log_area, appended_text_lands_after_the_header_padded_with_newlines) {
    format_area(16);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    CHECK_EQ(disk_log_area_append(&area, "hello\n", 6), 0);
    CHECK_EQ(disk_log_area_write_header(&area), 0);
    CHECK_MEMEQ(data_at(0), "hello\n", 6);
    CHECK_EQ(*data_at(6), '\n');
    CHECK_EQ(*data_at(511), '\n');

    uint32_t next = 99, wrapped = 99, boots = 99;
    CHECK_EQ(disk_log_area_parse_header(disk.sectors[AREA_LBA], &next, &wrapped, &boots), 0);
    CHECK_EQ(next, 6u);
    CHECK_EQ(wrapped, 0u);
    CHECK_EQ(boots, 1u);
    CHECK(!disk.touched_outside);
}

TEST(disk_log_area, a_second_append_keeps_the_partial_sector_the_first_left) {
    format_area(16);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    char text[600];
    pattern(text, sizeof(text), 0);
    CHECK_EQ(disk_log_area_append(&area, text, 300), 0);
    CHECK_EQ(disk_log_area_append(&area, text + 300, 300), 0);
    char back[600];
    copy_out(0, back, sizeof(back));
    CHECK_MEMEQ(back, text, sizeof(text));
    CHECK_EQ(area.next, 600u);
}

TEST(disk_log_area, an_append_larger_than_the_staging_buffer_is_contiguous) {
    format_area(200);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    static char text[DISK_LOG_AREA_STAGE_SECTORS * 512 * 2 + 77];
    static char back[sizeof(text)];
    pattern(text, sizeof(text), 3);
    CHECK_EQ(disk_log_area_append(&area, "x", 1), 0);
    CHECK_EQ(disk_log_area_append(&area, text, sizeof(text)), 0);
    copy_out(1, back, sizeof(back));
    CHECK_MEMEQ(back, text, sizeof(text));
    CHECK(disk.writes >= 3);
}

TEST(disk_log_area, a_second_boot_continues_where_the_first_stopped) {
    format_area(16);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    CHECK_EQ(disk_log_area_append(&area, "first boot\n", 11), 0);
    CHECK_EQ(disk_log_area_write_header(&area), 0);

    memset(&area, 0, sizeof(area));
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    CHECK_EQ(area.boots, 2u);
    CHECK_EQ(area.next, 11u);
    CHECK_EQ(disk_log_area_append(&area, "second boot\n", 12), 0);
    char back[23];
    copy_out(0, back, sizeof(back));
    CHECK_MEMEQ(back, "first boot\nsecond boot\n", 23);
}

TEST(disk_log_area, a_full_area_wraps_to_its_start_and_says_so) {
    format_area(5);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    char text[3000];
    pattern(text, sizeof(text), 7);
    CHECK_EQ(disk_log_area_append(&area, text, sizeof(text)), 0);
    CHECK_EQ(area.wrapped, 1u);
    CHECK_EQ(area.next, 3000u - 2048u);
    char back[952];
    copy_out(0, back, sizeof(back));
    CHECK_MEMEQ(back, text + 2048, sizeof(back));
    char older[1000];
    copy_out(1024, older, sizeof(older));
    CHECK_MEMEQ(older, text + 1024, sizeof(older));
    CHECK(!disk.touched_outside);
}

TEST(disk_log_area, ending_exactly_on_the_last_byte_wraps_without_writing_past_it) {
    format_area(5);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    char text[2048];
    pattern(text, sizeof(text), 1);
    CHECK_EQ(disk_log_area_append(&area, text, sizeof(text)), 0);
    CHECK_EQ(area.next, 0u);
    CHECK_EQ(area.wrapped, 1u);
    CHECK(!disk.touched_outside);
}

TEST(disk_log_area, a_header_pointing_past_the_end_starts_over_rather_than_writing_there) {
    format_area(5);
    memset(disk.sectors[AREA_LBA], '\n', 512);
    const char *header = "LEAN_OS LOG AREA 1\nnext=999999\nwrapped=0\nboots=4\n";
    memcpy(disk.sectors[AREA_LBA], header, strlen(header));
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    CHECK_EQ(area.next, 0u);
    CHECK_EQ(area.wrapped, 1u);
    CHECK_EQ(area.boots, 5u);
}

TEST(disk_log_area, a_failed_write_is_reported) {
    format_area(16);
    CHECK_EQ(open_area(), DISK_LOG_AREA_OPENED);
    disk.fail_writes = 1;
    CHECK_EQ(disk_log_area_append(&area, "lost\n", 5), -1);
    CHECK_EQ(disk_log_area_write_header(&area), -1);
}

static void parse_options(const char *text, boot_options_t *options) {
    boot_options_defaults(options);
    boot_options_parse(text, (uint32_t)strlen(text), options);
}

TEST(disk_log_area, the_boot_option_names_an_lba_and_a_sector_count) {
    boot_options_t options;
    parse_options("video=native\nlog=4280000+49152\n", &options);
    CHECK_EQ(options.log_lba, 4280000u);
    CHECK_EQ(options.log_sectors, 49152u);
    CHECK_EQ(options.unknown_keys, 0u);
}

TEST(disk_log_area, a_malformed_log_option_is_ignored_and_counted) {
    const char *bad[] = {"log=4280000\n", "log=+49152\n", "log=0+49152\n", "log=12+1\n",
                         "log=12+34x\n", "log=abc\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        boot_options_t options;
        parse_options(bad[i], &options);
        CHECK_EQ(options.log_sectors, 0u);
        CHECK_EQ(options.unknown_keys, 1u);
    }
}
