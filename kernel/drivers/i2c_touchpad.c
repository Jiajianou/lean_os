#include "i2c_touchpad.h"

#include <stdint.h>

#include "drivers/designware_i2c.h"
#include "drivers/hid_report.h"
#include "drivers/i2c_hid.h"
#include "drivers/kernel_log.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "scheduler/scheduler.h"

#define FIRST_ADDRESS 0x08
#define LAST_ADDRESS  0x77

#define POLL_INTERVAL_MS 8

static const uint16_t descriptor_registers[] = {0x0020, 0x0001, 0x0000};

static int found_controller = -1;
static uint8_t found_address;
static i2c_hid_descriptor_t descriptor;
static hid_mouse_layout_t layout;
static int present;

static uint8_t last_buttons;
static uint8_t input_buffer[256];
static uint8_t report_descriptor[4096];

static int read_at_register(int controller, uint8_t address, uint16_t register_address,
                            uint8_t *out, uint32_t length) {
    uint8_t command[2];
    uint32_t command_length = i2c_hid_build_register_address(register_address, command);
    return designware_i2c_transfer(controller, address, command, command_length, out, length);
}

static int address_answers(int controller, uint8_t address) {
    uint8_t byte = 0;
    return designware_i2c_transfer(controller, address, 0, 0, &byte, 1);
}

static int probe_address(int controller, uint8_t address) {
    for (uint32_t r = 0; r < sizeof(descriptor_registers) / sizeof(descriptor_registers[0]); r++) {
        uint8_t bytes[I2C_HID_DESCRIPTOR_LENGTH];
        if (!read_at_register(controller, address, descriptor_registers[r], bytes, sizeof(bytes))) {
            continue;
        }
        if (i2c_hid_parse_descriptor(bytes, sizeof(bytes), &descriptor)) {
            return 1;
        }
    }
    return 0;
}

static int send_command(uint8_t opcode, uint8_t argument) {
    uint8_t command[4];
    uint32_t length = i2c_hid_build_command(descriptor.command_register, opcode, argument, command);
    return designware_i2c_transfer(found_controller, found_address, command, length, 0, 0);
}

static i2c_touchpad_statistics_t statistics;

void i2c_touchpad_statistics(i2c_touchpad_statistics_t *out) {
    *out = statistics;
    out->report_id = layout.report_id;
}

/* A plain read, with no register written first. That is how HID over I2C
   hands over an input report and what Linux's i2c-hid does; writing the input
   register's address and then reading is a different transaction, and the
   first real touchpad this ran against answered it with nothing usable. */
static void poll_once(void) {
    uint32_t want = descriptor.max_input_length;
    if (want > sizeof(input_buffer)) {
        want = sizeof(input_buffer);
    }
    statistics.polls++;
    if (!designware_i2c_transfer(found_controller, found_address, 0, 0, input_buffer, want)) {
        statistics.read_failures++;
        return;
    }

    uint16_t length = i2c_hid_input_length(input_buffer, want);
    statistics.last_length = length;
    for (uint32_t i = 0; i < sizeof(statistics.last_bytes); i++) {
        statistics.last_bytes[i] = i < want ? input_buffer[i] : 0;
    }
    if (length <= 2 || length > want) {
        statistics.empty++;
        return;
    }

    hid_mouse_report_t report;
    if (!hid_mouse_decode(&layout, input_buffer + 2, (uint32_t)(length - 2), &report)) {
        statistics.wrong_report++;
        return;
    }
    statistics.reports++;

    if (report.dx == 0 && report.dy == 0 && report.wheel == 0 && report.buttons == last_buttons) {
        return;
    }
    last_buttons = report.buttons;
    statistics.injected++;
    mouse_inject(report.dx, report.dy, report.buttons, report.wheel);
}

static void poll_task(void *argument) {
    (void)argument;
    for (;;) {
        poll_once();
        pit_sleep_ms(POLL_INTERVAL_MS);
    }
}

int i2c_touchpad_present(void) {
    return present;
}

int i2c_touchpad_init(void) {
    present = 0;
    found_controller = -1;

    int controllers = designware_i2c_init();
    if (controllers == 0) {
        return 0;
    }

    for (int controller = 0; controller < controllers && found_controller < 0; controller++) {
        for (uint8_t address = FIRST_ADDRESS; address <= LAST_ADDRESS; address++) {
            if (!address_answers(controller, address)) {
                continue;
            }
            if (probe_address(controller, address)) {
                found_controller = controller;
                found_address = address;
                break;
            }
        }
    }

    if (found_controller < 0) {
        kernel_log_puts("[i2c-hid] no HID device answered on any I2C bus - nothing to drive.\n");
        return 0;
    }

    kernel_log_puts("[i2c-hid] device ");
    kernel_log_put_hex32(descriptor.vendor_id);
    kernel_log_puts(":");
    kernel_log_put_hex32(descriptor.product_id);
    kernel_log_puts(" at address 0x");
    kernel_log_put_hex32(found_address);
    kernel_log_puts(" on bus ");
    kernel_log_put_dec((uint32_t)found_controller);
    kernel_log_puts(".\n");

    send_command(I2C_HID_OPCODE_SET_POWER, I2C_HID_POWER_ON);
    send_command(I2C_HID_OPCODE_RESET, 0);
    pit_sleep_ms(100);

    uint32_t want = descriptor.report_descriptor_length;
    if (want > sizeof(report_descriptor)) {
        want = sizeof(report_descriptor);
    }
    if (!read_at_register(found_controller, found_address, descriptor.report_descriptor_register,
                          report_descriptor, want)) {
        kernel_log_puts("[i2c-hid] the device would not hand over its report descriptor.\n");
        return 0;
    }

    if (!hid_report_find_mouse(report_descriptor, want, &layout)) {
        kernel_log_puts("[i2c-hid] the report descriptor has no mouse collection in it - this "
                   "device is something other than a pointing device, or it is already in "
                   "its multitouch mode.\n");
        return 0;
    }

    kernel_log_puts("[i2c-hid] mouse collection: report id ");
    kernel_log_put_dec(layout.report_id);
    kernel_log_puts(", ");
    kernel_log_put_dec(layout.button_count);
    kernel_log_puts(" button(s), ");
    kernel_log_put_dec(layout.report_bits);
    kernel_log_puts(" bits, wheel ");
    kernel_log_puts(layout.has_wheel ? "yes" : "no");
    kernel_log_puts(".\n");

    present = 1;
    return 1;
}

/* The poll task cannot be spawned from i2c_touchpad_init, because the probe
   has to happen early enough for the boot inventory to report what it found
   and the scheduler does not exist yet at that point. */
void i2c_touchpad_start(void) {
    if (!present) {
        return;
    }
    /* Nobody's child. Spawned from the boot task, it would be the one child the
       SYS_wait self-test drains and never gets back - it hung the first boot
       that had a touchpad, which was the first boot on real hardware. */
    task_t *poller = task_spawn("i2c_touchpad", poll_task, 0);
    if (poller) {
        poller->parent_id = -1;
    }
}
