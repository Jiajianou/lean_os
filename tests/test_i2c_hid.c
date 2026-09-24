#include "check.h"

#include "drivers/designware_i2c_timing.h"
#include "drivers/i2c_hid.h"

#include <string.h>

static void build_descriptor(uint8_t *bytes) {
    static const uint16_t fields[13] = {
        30, 0x0100, 0x00B2, 0x0001, 0x0003, 0x000B, 0x0004, 0x0008,
        0x0005, 0x0006, 0x06CB, 0xCE57, 0x0100,
    };
    for (int i = 0; i < 13; i++) {
        bytes[i * 2] = (uint8_t)(fields[i] & 0xFF);
        bytes[i * 2 + 1] = (uint8_t)(fields[i] >> 8);
    }
    bytes[26] = bytes[27] = bytes[28] = bytes[29] = 0;
}

TEST(i2c_hid, a_real_descriptor_parses_into_its_registers) {
    uint8_t bytes[I2C_HID_DESCRIPTOR_LENGTH];
    build_descriptor(bytes);

    i2c_hid_descriptor_t descriptor;
    CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor), 1);
    CHECK_EQ(descriptor.version, 0x0100);
    CHECK_EQ(descriptor.report_descriptor_length, 0xB2);
    CHECK_EQ(descriptor.input_register, 0x0003);
    CHECK_EQ(descriptor.command_register, 0x0005);
    CHECK_EQ(descriptor.vendor_id, 0x06CB);
}

TEST(i2c_hid, the_probe_refuses_everything_that_is_not_one) {
    uint8_t bytes[I2C_HID_DESCRIPTOR_LENGTH];
    i2c_hid_descriptor_t descriptor;

    memset(bytes, 0x00, sizeof(bytes));
    CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor), 0);

    memset(bytes, 0xFF, sizeof(bytes));
    CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor), 0);

    build_descriptor(bytes);
    bytes[0] = 32;
    CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor), 0);

    build_descriptor(bytes);
    bytes[2] = 0x02;
    CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor), 0);

    build_descriptor(bytes);
    bytes[8] = 0; bytes[9] = 0;
    CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor), 0);

    build_descriptor(bytes);
    CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes) - 1, &descriptor), 0);
}

TEST(i2c_hid, a_bus_of_ascending_bytes_is_not_mistaken_for_a_device) {
    uint8_t bytes[I2C_HID_DESCRIPTOR_LENGTH];
    i2c_hid_descriptor_t descriptor;
    for (int start = 0; start < 256; start++) {
        for (int i = 0; i < I2C_HID_DESCRIPTOR_LENGTH; i++) {
            bytes[i] = (uint8_t)(start + i);
        }
        CHECK_EQ(i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor), 0);
    }
}

TEST(i2c_hid, a_command_is_the_register_then_the_argument_then_the_opcode) {
    uint8_t out[4];
    CHECK_EQ(i2c_hid_build_command(0x0005, I2C_HID_OPCODE_RESET, 0, out), 4u);
    CHECK_EQ(out[0], 0x05);
    CHECK_EQ(out[1], 0x00);
    CHECK_EQ(out[2], 0x00);
    CHECK_EQ(out[3], 0x01);

    CHECK_EQ(i2c_hid_build_command(0x0105, I2C_HID_OPCODE_SET_POWER, I2C_HID_POWER_ON, out), 4u);
    CHECK_EQ(out[0], 0x05);
    CHECK_EQ(out[1], 0x01);
    CHECK_EQ(out[3], 0x08);
}

TEST(i2c_hid, a_register_address_goes_out_little_endian) {
    uint8_t out[2];
    CHECK_EQ(i2c_hid_build_register_address(0x0020, out), 2u);
    CHECK_EQ(out[0], 0x20);
    CHECK_EQ(out[1], 0x00);
}

TEST(i2c_hid, an_empty_read_is_how_the_device_says_nothing_happened) {
    const uint8_t nothing[2] = {0x00, 0x00};
    CHECK_EQ(i2c_hid_input_length(nothing, sizeof(nothing)), 0);
    CHECK_EQ(i2c_hid_input_is_reset_acknowledgement(nothing, sizeof(nothing)), 1);

    const uint8_t report[6] = {0x06, 0x00, 0x01, 0x00, 0x03, 0xFD};
    CHECK_EQ(i2c_hid_input_length(report, sizeof(report)), 6);
    CHECK_EQ(i2c_hid_input_is_reset_acknowledgement(report, sizeof(report)), 0);
}

TEST(i2c, the_scl_counts_are_the_ones_linux_computes_for_the_same_clock) {
    uint16_t high = 0, low = 0;

    designware_i2c_counts(133000, &high, &low);
    CHECK_EQ(high, 117);
    CHECK_EQ(low, 212);

    designware_i2c_counts(216000, &high, &low);
    CHECK_EQ(high, 191);
    CHECK_EQ(low, 345);

    designware_i2c_counts(100000, &high, &low);
    CHECK_EQ(high, 87);
    CHECK_EQ(low, 159);
}

TEST(i2c, counts_for_the_fastest_part_never_overclock_a_slower_one) {
    uint16_t high = 0, low = 0;
    designware_i2c_counts(216000, &high, &low);
    uint32_t period = (uint32_t)high + low + 8;

    static const uint32_t intel_lpss_clocks_khz[] = {100000, 120000, 133000, 216000};
    for (unsigned i = 0; i < sizeof(intel_lpss_clocks_khz) / sizeof(intel_lpss_clocks_khz[0]); i++) {
        uint32_t scl_khz = intel_lpss_clocks_khz[i] / period;
        CHECK(scl_khz <= 400);
        CHECK(scl_khz >= 100);
    }
}
