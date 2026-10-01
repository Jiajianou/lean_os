#include "check.h"

#include <stdlib.h>

#include "drivers/intel_wireless_firmware.h"

typedef struct {
    uint8_t bytes[4096];
    uint32_t length;
} image_t;

static void put32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static void begin(image_t *image) {
    memset(image, 0, sizeof(*image));
    put32(image->bytes + 4, INTEL_WIRELESS_FIRMWARE_MAGIC);
    memcpy(image->bytes + 8, "release/core74::563a6e92", 24);
    put32(image->bytes + 72, 77);
    image->length = 88;
}

static void record(image_t *image, uint32_t type, const uint8_t *data, uint32_t length) {
    put32(image->bytes + image->length, type);
    put32(image->bytes + image->length + 4, length);
    memcpy(image->bytes + image->length + 8, data, length);
    image->length += 8 + ((length + 3) & ~3u);
}

static void section(image_t *image, uint32_t device_offset, uint32_t payload_length, uint8_t fill) {
    uint8_t data[256];
    put32(data, device_offset);
    memset(data + 4, fill, payload_length);
    record(image, 19, data, 4 + payload_length);
}

static void word_pair(image_t *image, uint32_t type, uint32_t index, uint32_t bits) {
    uint8_t data[8];
    put32(data, index);
    put32(data + 4, bits);
    record(image, type, data, 8);
}

static void word(image_t *image, uint32_t type, uint32_t value) {
    uint8_t data[4];
    put32(data, value);
    record(image, type, data, 4);
}

static void a_real_shaped_image(image_t *image) {
    begin(image);
    word(image, 23, 0x00330018u);
    word_pair(image, 30, 0, 0x9DEF037Fu);
    word_pair(image, 30, 2, 0x00000010u);
    word_pair(image, 29, 1, 0xFFF7FFFFu);
    word(image, 31, 0x43);
    section(image, 0x433000, 10, 0x11);
    section(image, 0x800000, 7, 0x12);
    section(image, INTEL_WIRELESS_LMAC_UMAC_SEPARATOR, 0, 0);
    section(image, 0x80433000, 9, 0x21);
    section(image, INTEL_WIRELESS_PAGING_SEPARATOR, 0, 0);
    section(image, 0x0, 6, 0x31);
    section(image, 0x1000000, 5, 0x32);
    section(image, 0x1008000, 3, 0x33);
    uint8_t versions[8] = {0x0d, 0x01, 15, 0, 0x02, 0x0c, 1, 4};
    record(image, 48, versions, sizeof(versions));
}

TEST(intel_wireless_firmware, sections_split_at_the_two_separators) {
    image_t image;
    a_real_shaped_image(&image);
    intel_wireless_firmware_t firmware;
    REQUIRE(intel_wireless_firmware_parse(image.bytes, image.length, &firmware) == 0);
    CHECK_EQ(firmware.lmac_count, 2u);
    CHECK_EQ(firmware.umac_count, 1u);
    CHECK_EQ(firmware.paging_count, 3u);
    CHECK_EQ(firmware.lmac[0].device_offset, 0x433000u);
    CHECK_EQ(firmware.lmac[0].length, 10u);
    CHECK_EQ(firmware.lmac[0].data[0], 0x11);
    CHECK_EQ(firmware.lmac[1].length, 7u);
    CHECK_EQ(firmware.umac[0].device_offset, 0x80433000u);
    CHECK_EQ(firmware.umac[0].data[8], 0x21);
    CHECK_EQ(firmware.paging[2].device_offset, 0x1008000u);
    CHECK_EQ(firmware.paging[2].length, 3u);
}

TEST(intel_wireless_firmware, the_antennas_come_out_of_the_phy_sku) {
    image_t image;
    a_real_shaped_image(&image);
    intel_wireless_firmware_t firmware;
    REQUIRE(intel_wireless_firmware_parse(image.bytes, image.length, &firmware) == 0);
    CHECK_EQ(firmware.phy_config, 0x00330018u);
    CHECK_EQ(firmware.valid_tx_antennas, 3);
    CHECK_EQ(firmware.valid_rx_antennas, 3);
    CHECK_EQ(firmware.scan_channels, 0x43u);
    CHECK_EQ(strcmp(firmware.human_readable, "release/core74::563a6e92"), 0);
    CHECK_EQ(firmware.api_version, 77u);
}

TEST(intel_wireless_firmware, capability_words_are_indexed_by_their_record) {
    image_t image;
    a_real_shaped_image(&image);
    intel_wireless_firmware_t firmware;
    REQUIRE(intel_wireless_firmware_parse(image.bytes, image.length, &firmware) == 0);
    CHECK(intel_wireless_firmware_has_capability(&firmware, 0));
    CHECK(!intel_wireless_firmware_has_capability(&firmware, 7));
    CHECK(intel_wireless_firmware_has_capability(&firmware, 64 + 4));
    CHECK(!intel_wireless_firmware_has_capability(&firmware, 32 + 4));
    CHECK(intel_wireless_firmware_has_api(&firmware, 32));
    CHECK(!intel_wireless_firmware_has_api(&firmware, 32 + 19));
    CHECK(!intel_wireless_firmware_has_api(&firmware, 0));
    CHECK(!intel_wireless_firmware_has_capability(&firmware, 100000));
}

TEST(intel_wireless_firmware, a_legacy_command_is_found_under_the_long_group) {
    image_t image;
    a_real_shaped_image(&image);
    intel_wireless_firmware_t firmware;
    REQUIRE(intel_wireless_firmware_parse(image.bytes, image.length, &firmware) == 0);
    CHECK_EQ(intel_wireless_command_version(&firmware, 0, 0x0d, 0), 15);
    CHECK_EQ(intel_wireless_command_version(&firmware, 1, 0x0d, 0), 15);
    CHECK_EQ(intel_wireless_notification_version(&firmware, 0x0c, 0x02, 0), 4);
    CHECK_EQ(intel_wireless_command_version(&firmware, 0x0c, 0x02, 0), 1);
    CHECK_EQ(intel_wireless_command_version(&firmware, 0, 0x28, 99), 99);
    CHECK_EQ(intel_wireless_command_version(&firmware, 5, 0x0d, 7), 7);
}

TEST(intel_wireless_firmware, a_truncated_file_is_refused_not_overread) {
    image_t image;
    a_real_shaped_image(&image);
    intel_wireless_firmware_t firmware;
    for (uint32_t cut = 0; cut < image.length; cut++) {
        uint8_t *copy = malloc(cut ? cut : 1);
        memcpy(copy, image.bytes, cut);
        int result = intel_wireless_firmware_parse(copy, cut, &firmware);
        free(copy);
        if (result == 0) {
            CHECK_MSG(firmware.lmac_count > 0 && firmware.umac_count > 0,
                      "a cut at %u parsed without its sections", cut);
        }
    }
    CHECK(intel_wireless_firmware_parse(image.bytes, 87, &firmware) < 0);
}

TEST(intel_wireless_firmware, the_wrong_magic_is_somebody_elses_file) {
    image_t image;
    a_real_shaped_image(&image);
    intel_wireless_firmware_t firmware;
    image.bytes[4] ^= 1;
    CHECK_EQ(intel_wireless_firmware_parse(image.bytes, image.length, &firmware), -4);
    image.bytes[4] ^= 1;
    image.bytes[0] = 1;
    CHECK_EQ(intel_wireless_firmware_parse(image.bytes, image.length, &firmware), -3);
}

TEST(intel_wireless_firmware, a_record_longer_than_the_file_is_refused) {
    image_t image;
    a_real_shaped_image(&image);
    put32(image.bytes + 88 + 4, 0x10000);
    intel_wireless_firmware_t firmware;
    CHECK_EQ(intel_wireless_firmware_parse(image.bytes, image.length, &firmware), -6);
}

TEST(intel_wireless_firmware, separators_out_of_order_are_refused) {
    image_t image;
    begin(&image);
    section(&image, 0x433000, 4, 1);
    section(&image, INTEL_WIRELESS_PAGING_SEPARATOR, 0, 0);
    intel_wireless_firmware_t firmware;
    CHECK_EQ(intel_wireless_firmware_parse(image.bytes, image.length, &firmware), -22);
}

TEST(intel_wireless_firmware, an_image_with_no_umac_cannot_start) {
    image_t image;
    begin(&image);
    section(&image, 0x433000, 4, 1);
    section(&image, INTEL_WIRELESS_LMAC_UMAC_SEPARATOR, 0, 0);
    intel_wireless_firmware_t firmware;
    CHECK_EQ(intel_wireless_firmware_parse(image.bytes, image.length, &firmware), -13);
}

TEST(intel_wireless_firmware, the_file_is_named_the_way_linux_names_it) {
    char name[64];
    CHECK(intel_wireless_firmware_name(0x00000350u, 0x0010A100u, name, sizeof(name)) > 0);
    CHECK_EQ(strcmp(name, "iwlwifi-QuZ-a0-hr-b0-77.ucode"), 0);
    CHECK(intel_wireless_firmware_name(0x00000331u, 0x0010C000u, name, sizeof(name)) > 0);
    CHECK_EQ(strcmp(name, "iwlwifi-Qu-b0-hr-b0-77.ucode"), 0);
    CHECK(intel_wireless_firmware_name(0x00000332u, 0x0010A000u, name, sizeof(name)) > 0);
    CHECK_EQ(strcmp(name, "iwlwifi-Qu-c0-hr-b0-77.ucode"), 0);
    CHECK_EQ(intel_wireless_firmware_name(0x00000420u, 0x0010A000u, name, sizeof(name)), 0);
    CHECK_EQ(intel_wireless_firmware_name(0x00000350u, 0x00105000u, name, sizeof(name)), 0);
    CHECK_EQ(intel_wireless_firmware_name(0x00000350u, 0x0010A000u, name, 10), 0);
}
