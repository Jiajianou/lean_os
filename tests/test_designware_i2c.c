#include "check.h"
#include "fakes.h"

#include "drivers/designware_i2c.h"
#include "drivers/hid_report.h"
#include "drivers/i2c_hid.h"

#include <stdint.h>
#include <string.h>

#define REGISTER_CONTROL         0x00
#define REGISTER_TARGET_ADDRESS  0x04
#define REGISTER_DATA_COMMAND    0x10
#define REGISTER_RAW_INTERRUPT   0x34
#define REGISTER_CLEAR_TX_ABORT  0x54
#define REGISTER_ENABLE          0x6C
#define REGISTER_STATUS          0x70
#define REGISTER_RX_LEVEL        0x78
#define REGISTER_ENABLE_STATUS   0x9C
#define REGISTER_COMPONENT_PARAM 0xF4
#define REGISTER_COMPONENT_TYPE  0xFC

#define LPSS_PRIVATE_RESETS 0x204

#define STATUS_TX_FIFO_NOT_FULL  (1u << 1)
#define STATUS_RX_FIFO_NOT_EMPTY (1u << 3)

#define DATA_COMMAND_READ (1u << 8)

#define RAW_INTERRUPT_TX_ABORT (1u << 6)

#define CONTROLLER_BASE 0xFE000000ULL

#define SLAVE_ADDRESS 0x2C

#define HID_DESCRIPTOR_REGISTER 0x0020
#define INPUT_REGISTER          0x0003
#define REPORT_DESCRIPTOR_REGISTER 0x0001
#define COMMAND_REGISTER        0x0005

static const uint8_t mouse_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0,
};

typedef struct {
    int enabled;
    int in_reset;
    uint8_t target;
    uint8_t write_bytes[64];
    uint32_t write_length;
    uint8_t read_queue[512];
    uint32_t read_head;
    uint32_t read_tail;
    int aborted;
    uint32_t transfers;
    uint32_t reads_asked;
    uint32_t reads_taken;
    uint32_t delivered;
    uint32_t clock;
    int overrun;
} model_t;

/* A real receive queue is eight entries here so that a read longer than it
   has to be paced: a byte asked for with the queue already full is a byte the
   controller drops. */
#define MODEL_RECEIVE_DEPTH 8u

static model_t model;
static uint8_t slave_input_report[8];
static uint32_t slave_input_length;

static void queue_byte(uint8_t byte) {
    if (model.read_tail < sizeof(model.read_queue)) {
        model.read_queue[model.read_tail++] = byte;
    }
}

static uint16_t addressed_register(void) {
    if (model.write_length < 2) {
        return 0xFFFF;
    }
    return (uint16_t)(model.write_bytes[0] | ((uint16_t)model.write_bytes[1] << 8));
}

static void slave_produce(void) {
    uint16_t reg = addressed_register();

    if (model.write_length == 0) {
        queue_byte(0x00);
        return;
    }
    if (reg == HID_DESCRIPTOR_REGISTER) {
        static const uint16_t fields[13] = {
            30, 0x0100, (uint16_t)sizeof(mouse_report_descriptor), REPORT_DESCRIPTOR_REGISTER,
            INPUT_REGISTER, 0x000B, 0x0004, 0x0008, COMMAND_REGISTER, 0x0006,
            0x06CB, 0xCE57, 0x0100,
        };
        for (int i = 0; i < 13; i++) {
            queue_byte((uint8_t)(fields[i] & 0xFF));
            queue_byte((uint8_t)(fields[i] >> 8));
        }
        for (int i = 0; i < 4; i++) {
            queue_byte(0);
        }
        return;
    }
    if (reg == REPORT_DESCRIPTOR_REGISTER) {
        for (uint32_t i = 0; i < sizeof(mouse_report_descriptor); i++) {
            queue_byte(mouse_report_descriptor[i]);
        }
        return;
    }
    if (reg == INPUT_REGISTER) {
        for (uint32_t i = 0; i < slave_input_length; i++) {
            queue_byte(slave_input_report[i]);
        }
        return;
    }
    for (int i = 0; i < 64; i++) {
        queue_byte(0xFF);
    }
}

/* The bus is slower than the processor asking it: a byte arrives every few
   register reads rather than the moment it is asked for, which is what lets a
   driver that does not pace itself get ahead of the receive queue. */
static void bus_tick(void) {
    if (++model.clock % 8 == 0 && model.delivered < model.reads_asked) {
        model.delivered++;
    }
}

static uint32_t receive_level(void) {
    uint32_t level = model.delivered - model.reads_taken;
    uint32_t queued = model.read_tail - model.read_head;
    return level < queued ? level : queued;
}

uint32_t designware_i2c_host_read(uint64_t base, uint32_t offset) {
    if (base != CONTROLLER_BASE) {
        return 0;
    }
    bus_tick();
    switch (offset) {
        case REGISTER_COMPONENT_TYPE:
            return model.in_reset ? 0 : DESIGNWARE_I2C_COMPONENT_TYPE;
        case REGISTER_COMPONENT_PARAM:
            return (63u << 16) | ((MODEL_RECEIVE_DEPTH - 1u) << 8);
        case REGISTER_ENABLE_STATUS:
            return model.enabled ? 1u : 0u;
        case REGISTER_STATUS: {
            uint32_t status = STATUS_TX_FIFO_NOT_FULL;
            if (receive_level() > 0) {
                status |= STATUS_RX_FIFO_NOT_EMPTY;
            }
            return status;
        }
        case REGISTER_RX_LEVEL:
            return receive_level();
        case REGISTER_DATA_COMMAND:
            if (receive_level() > 0) {
                model.reads_taken++;
                return model.read_queue[model.read_head++];
            }
            return 0;
        case REGISTER_RAW_INTERRUPT:
            return model.aborted ? RAW_INTERRUPT_TX_ABORT : 0u;
        case REGISTER_CLEAR_TX_ABORT:
            model.aborted = 0;
            return 0;
        default:
            return 0;
    }
}

void designware_i2c_host_write(uint64_t base, uint32_t offset, uint32_t value) {
    if (base != CONTROLLER_BASE) {
        return;
    }
    switch (offset) {
        case LPSS_PRIVATE_RESETS:
            model.in_reset = (value == 0);
            break;
        case REGISTER_ENABLE:
            if (value & 1u) {
                model.enabled = 1;
                model.write_length = 0;
                model.read_head = 0;
                model.read_tail = 0;
                model.aborted = 0;
                model.reads_asked = 0;
                model.reads_taken = 0;
                model.delivered = 0;
            } else {
                model.enabled = 0;
            }
            break;
        case REGISTER_TARGET_ADDRESS:
            model.target = (uint8_t)(value & 0x7Fu);
            break;
        case REGISTER_DATA_COMMAND:
            model.transfers++;
            if (model.target != SLAVE_ADDRESS) {
                model.aborted = 1;
                break;
            }
            if (value & DATA_COMMAND_READ) {
                if (++model.reads_asked - model.reads_taken > MODEL_RECEIVE_DEPTH) {
                    model.overrun = 1;
                }
                if (model.read_tail == 0) {
                    slave_produce();
                }
            } else if (model.write_length < sizeof(model.write_bytes)) {
                model.write_bytes[model.write_length++] = (uint8_t)(value & 0xFFu);
            }
            break;
        default:
            break;
    }
}

static void reset_model(int held_in_reset) {
    memset(&model, 0, sizeof(model));
    model.in_reset = held_in_reset;
    slave_input_length = 0;

    fake_pci_reset();
    int handle = fake_pci_add(0, 21, 0, 0x8086, 0xA0E8, 0x0C, 0x80, 0x00);
    fake_pci_set_bar(handle, 0, (uint32_t)CONTROLLER_BASE, 0xFFFFF000u);
}

TEST(designware_i2c, a_serial_bus_controller_that_identifies_itself_is_taken) {
    reset_model(0);
    CHECK_EQ(designware_i2c_init(), 1);
    CHECK_EQ(designware_i2c_controller_count(), 1);
}

TEST(designware_i2c, a_controller_still_held_in_reset_identifies_as_nothing_until_released) {
    reset_model(1);
    CHECK_EQ(designware_i2c_init(), 1);
    CHECK_EQ(model.in_reset, 0);
}

TEST(designware_i2c, a_class_0c80_device_that_is_not_designware_is_left_alone) {
    reset_model(0);
    fake_pci_reset();
    int handle = fake_pci_add(0, 21, 0, 0x1234, 0x5678, 0x0C, 0x80, 0x00);
    fake_pci_set_bar(handle, 0, 0xFD000000u, 0xFFFFF000u);
    CHECK_EQ(designware_i2c_init(), 0);
}

TEST(designware_i2c, an_address_nothing_answers_on_aborts_rather_than_hanging) {
    reset_model(0);
    REQUIRE(designware_i2c_init() == 1);

    uint8_t byte = 0;
    CHECK_EQ(designware_i2c_transfer(0, 0x10, 0, 0, &byte, 1), 0);
    CHECK_EQ(designware_i2c_transfer(0, SLAVE_ADDRESS, 0, 0, &byte, 1), 1);
}

TEST(designware_i2c, a_write_then_read_gets_the_register_the_write_named) {
    reset_model(0);
    REQUIRE(designware_i2c_init() == 1);

    const uint8_t address[2] = {0x20, 0x00};
    uint8_t descriptor[30];
    memset(descriptor, 0, sizeof(descriptor));
    CHECK_EQ(designware_i2c_transfer(0, SLAVE_ADDRESS, address, 2, descriptor, 30), 1);

    CHECK_EQ(descriptor[0], 30);
    CHECK_EQ(descriptor[1], 0);
    CHECK_EQ(descriptor[2], 0x00);
    CHECK_EQ(descriptor[3], 0x01);
    CHECK_EQ(descriptor[20], 0xCB);
    CHECK_EQ(descriptor[21], 0x06);
}

TEST(designware_i2c, a_read_longer_than_the_fifo_still_comes_back_whole) {
    reset_model(0);
    REQUIRE(designware_i2c_init() == 1);

    const uint8_t address[2] = {0x01, 0x00};
    uint8_t descriptor[sizeof(mouse_report_descriptor)];
    memset(descriptor, 0, sizeof(descriptor));
    CHECK_EQ(designware_i2c_transfer(0, SLAVE_ADDRESS, address, 2, descriptor,
                                     sizeof(mouse_report_descriptor)), 1);
    CHECK_EQ(memcmp(descriptor, mouse_report_descriptor, sizeof(mouse_report_descriptor)), 0);
    CHECK_EQ(model.overrun, 0);
}

TEST(designware_i2c, a_write_with_no_read_leaves_the_bytes_at_the_slave) {
    reset_model(0);
    REQUIRE(designware_i2c_init() == 1);

    const uint8_t command[4] = {0x05, 0x00, 0x00, 0x01};
    CHECK_EQ(designware_i2c_transfer(0, SLAVE_ADDRESS, command, 4, 0, 0), 1);
    CHECK_EQ(model.write_length, 4u);
    CHECK_EQ(model.write_bytes[3], 0x01);
}

TEST(designware_i2c, a_transfer_to_a_controller_that_is_not_there_refuses) {
    reset_model(0);
    REQUIRE(designware_i2c_init() == 1);

    uint8_t byte = 0;
    CHECK_EQ(designware_i2c_transfer(1, SLAVE_ADDRESS, 0, 0, &byte, 1), 0);
    CHECK_EQ(designware_i2c_transfer(-1, SLAVE_ADDRESS, 0, 0, &byte, 1), 0);
}

TEST(designware_i2c, a_simulated_touchpad_becomes_a_mouse_movement) {
    reset_model(0);
    REQUIRE(designware_i2c_init() == 1);

    uint8_t address[2];
    uint32_t address_length = i2c_hid_build_register_address(HID_DESCRIPTOR_REGISTER, address);

    uint8_t descriptor_bytes[I2C_HID_DESCRIPTOR_LENGTH];
    REQUIRE(designware_i2c_transfer(0, SLAVE_ADDRESS, address, address_length, descriptor_bytes,
                                    sizeof(descriptor_bytes)) == 1);

    i2c_hid_descriptor_t descriptor;
    REQUIRE(i2c_hid_parse_descriptor(descriptor_bytes, sizeof(descriptor_bytes), &descriptor) == 1);
    CHECK_EQ(descriptor.vendor_id, 0x06CB);
    CHECK_EQ(descriptor.product_id, 0xCE57);

    uint8_t command[4];
    uint32_t command_length = i2c_hid_build_command(descriptor.command_register,
                                                    I2C_HID_OPCODE_RESET, 0, command);
    CHECK_EQ(designware_i2c_transfer(0, SLAVE_ADDRESS, command, command_length, 0, 0), 1);

    address_length = i2c_hid_build_register_address(descriptor.report_descriptor_register, address);
    uint8_t report_descriptor[256];
    REQUIRE(descriptor.report_descriptor_length <= sizeof(report_descriptor));
    REQUIRE(designware_i2c_transfer(0, SLAVE_ADDRESS, address, address_length, report_descriptor,
                                    descriptor.report_descriptor_length) == 1);

    hid_mouse_layout_t layout;
    REQUIRE(hid_report_find_mouse(report_descriptor, descriptor.report_descriptor_length,
                                  &layout) == 1);
    CHECK_EQ(layout.report_id, 1);
    CHECK_EQ(layout.button_count, 2);

    slave_input_report[0] = 0x06;
    slave_input_report[1] = 0x00;
    slave_input_report[2] = 0x01;
    slave_input_report[3] = 0x01;
    slave_input_report[4] = 0x0C;
    slave_input_report[5] = 0xF9;
    slave_input_length = 6;

    address_length = i2c_hid_build_register_address(descriptor.input_register, address);
    uint8_t input[16];
    memset(input, 0, sizeof(input));
    REQUIRE(designware_i2c_transfer(0, SLAVE_ADDRESS, address, address_length, input, 6) == 1);

    uint16_t length = i2c_hid_input_length(input, sizeof(input));
    CHECK_EQ(length, 6);

    hid_mouse_report_t decoded;
    REQUIRE(hid_mouse_decode(&layout, input + 2, (uint32_t)(length - 2), &decoded) == 1);
    CHECK_EQ(decoded.buttons, 1);
    CHECK_EQ(decoded.dx, 12);
    CHECK_EQ(decoded.dy, -7);
}

TEST(designware_i2c, a_touchpad_with_nothing_to_say_reports_a_zero_length) {
    reset_model(0);
    REQUIRE(designware_i2c_init() == 1);

    slave_input_report[0] = 0x00;
    slave_input_report[1] = 0x00;
    slave_input_length = 2;

    uint8_t address[2];
    uint32_t address_length = i2c_hid_build_register_address(INPUT_REGISTER, address);
    uint8_t input[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    REQUIRE(designware_i2c_transfer(0, SLAVE_ADDRESS, address, address_length, input, 2) == 1);
    CHECK_EQ(i2c_hid_input_length(input, 2), 0);
}
