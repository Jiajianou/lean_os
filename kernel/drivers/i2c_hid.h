#pragma once

#include <stdint.h>

#define I2C_HID_DESCRIPTOR_LENGTH 30

#define I2C_HID_OPCODE_RESET      0x01
#define I2C_HID_OPCODE_SET_POWER  0x08

#define I2C_HID_POWER_ON    0x00
#define I2C_HID_POWER_SLEEP 0x01

typedef struct {
    uint16_t descriptor_length;
    uint16_t version;
    uint16_t report_descriptor_length;
    uint16_t report_descriptor_register;
    uint16_t input_register;
    uint16_t max_input_length;
    uint16_t output_register;
    uint16_t max_output_length;
    uint16_t command_register;
    uint16_t data_register;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version_id;
} i2c_hid_descriptor_t;

int i2c_hid_parse_descriptor(const uint8_t *bytes, uint32_t length, i2c_hid_descriptor_t *out);

uint32_t i2c_hid_build_register_address(uint16_t register_address, uint8_t *out);

uint32_t i2c_hid_build_command(uint16_t command_register, uint8_t opcode, uint8_t argument,
                               uint8_t *out);

uint16_t i2c_hid_input_length(const uint8_t *buffer, uint32_t length);

int i2c_hid_input_is_reset_acknowledgement(const uint8_t *buffer, uint32_t length);
