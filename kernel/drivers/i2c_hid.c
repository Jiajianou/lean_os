#include "i2c_hid.h"

static uint16_t read16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

int i2c_hid_parse_descriptor(const uint8_t *bytes, uint32_t length, i2c_hid_descriptor_t *out) {
    if (!bytes || !out || length < I2C_HID_DESCRIPTOR_LENGTH) {
        return 0;
    }

    i2c_hid_descriptor_t d;
    d.descriptor_length = read16(bytes + 0);
    d.version = read16(bytes + 2);
    d.report_descriptor_length = read16(bytes + 4);
    d.report_descriptor_register = read16(bytes + 6);
    d.input_register = read16(bytes + 8);
    d.max_input_length = read16(bytes + 10);
    d.output_register = read16(bytes + 12);
    d.max_output_length = read16(bytes + 14);
    d.command_register = read16(bytes + 16);
    d.data_register = read16(bytes + 18);
    d.vendor_id = read16(bytes + 20);
    d.product_id = read16(bytes + 22);
    d.version_id = read16(bytes + 24);

    if (d.descriptor_length != I2C_HID_DESCRIPTOR_LENGTH) {
        return 0;
    }
    if (d.version != 0x0100) {
        return 0;
    }
    if (d.report_descriptor_length == 0 || d.report_descriptor_length > 4096) {
        return 0;
    }
    if (d.max_input_length < 2 || d.max_input_length > 1024) {
        return 0;
    }
    if (d.report_descriptor_register == 0 || d.report_descriptor_register == 0xFFFF) {
        return 0;
    }
    if (d.input_register == 0 || d.input_register == 0xFFFF) {
        return 0;
    }
    if (d.command_register == 0 || d.command_register == 0xFFFF) {
        return 0;
    }
    if (d.data_register == 0 || d.data_register == 0xFFFF) {
        return 0;
    }
    if (d.vendor_id == 0 || d.vendor_id == 0xFFFF) {
        return 0;
    }

    *out = d;
    return 1;
}

uint32_t i2c_hid_build_register_address(uint16_t register_address, uint8_t *out) {
    out[0] = (uint8_t)(register_address & 0xFFu);
    out[1] = (uint8_t)(register_address >> 8);
    return 2;
}

uint32_t i2c_hid_build_command(uint16_t command_register, uint8_t opcode, uint8_t argument,
                               uint8_t *out) {
    out[0] = (uint8_t)(command_register & 0xFFu);
    out[1] = (uint8_t)(command_register >> 8);
    out[2] = argument;
    out[3] = opcode;
    return 4;
}

uint16_t i2c_hid_input_length(const uint8_t *buffer, uint32_t length) {
    if (!buffer || length < 2) {
        return 0;
    }
    return read16(buffer);
}

int i2c_hid_input_is_reset_acknowledgement(const uint8_t *buffer, uint32_t length) {
    return i2c_hid_input_length(buffer, length) == 0;
}

/* SET_REPORT is a command and its data in one write: the command register,
   the opcode with the report type and id packed into one byte (an id of 15
   or more does not fit and follows on its own), then the data register and
   a length that counts itself. */
uint32_t i2c_hid_build_set_feature(uint16_t command_register, uint16_t data_register, uint8_t report_id,
                                   const uint8_t *payload, uint32_t payload_length, uint8_t *out,
                                   uint32_t capacity) {
    uint32_t need = 4u + (report_id >= 0x0F ? 1u : 0u) + 4u + (report_id ? 1u : 0u) + payload_length;
    if (!out || need > capacity || payload_length > 0xFFF0u) {
        return 0;
    }
    uint32_t at = 0;
    out[at++] = (uint8_t)(command_register & 0xFFu);
    out[at++] = (uint8_t)(command_register >> 8);
    out[at++] = (uint8_t)((I2C_HID_REPORT_TYPE_FEATURE << 4) | (report_id >= 0x0F ? 0x0Fu : report_id));
    out[at++] = I2C_HID_OPCODE_SET_REPORT;
    if (report_id >= 0x0F) {
        out[at++] = report_id;
    }
    out[at++] = (uint8_t)(data_register & 0xFFu);
    out[at++] = (uint8_t)(data_register >> 8);
    uint32_t size = 2u + (report_id ? 1u : 0u) + payload_length;
    out[at++] = (uint8_t)(size & 0xFFu);
    out[at++] = (uint8_t)(size >> 8);
    if (report_id) {
        out[at++] = report_id;
    }
    for (uint32_t i = 0; i < payload_length; i++) {
        out[at++] = payload[i];
    }
    return at;
}
