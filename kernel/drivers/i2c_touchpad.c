#include "i2c_touchpad.h"

#include <stdint.h>

#include "drivers/designware_i2c.h"
#include "drivers/hid_report.h"
#include "drivers/i2c_hid.h"
#include "drivers/kernel_log.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "drivers/touchpad_gestures.h"
#include "scheduler/scheduler.h"

#define FIRST_ADDRESS 0x08
#define LAST_ADDRESS  0x77

#define POLL_INTERVAL_MS 8
#define RESTING_POLL_INTERVAL_MS 25
#define RESTING_AFTER_MS 500

static const uint16_t descriptor_registers[] = {0x0020, 0x0001, 0x0000};

static int found_controller = -1;
static uint8_t found_address;
static i2c_hid_descriptor_t descriptor;
static hid_mouse_layout_t layout;
static int present;
static int have_mouse_layout;

/* M211. The pad is driven in its Precision Touchpad mode when its report
   descriptor offers one, because the mouse collection every touchpad starts
   in is the firmware's own idea of a mouse: the laptop's Elan pad reports two
   buttons and no wheel through it, so there was no scrolling on it at all and
   nothing for a scroll-direction setting to change. In touchpad mode it
   reports fingers, and two of them moving is a scroll, a quick touch is a
   click and a press with two down is the secondary button - the gestures live
   in touchpad_gestures.c, where the host tests can reach them. */
static hid_touchpad_layout_t touchpad_layout;
static int precision;
static touchpad_gestures_t gestures;
static uint32_t undecodable_in_a_row;

#define UNDECODABLE_BEFORE_MOUSE_MODE 64

static uint8_t last_buttons;
static uint8_t input_buffer[256];
static uint8_t report_descriptor[4096];

/* The whole report descriptor goes to the log, because it is the one thing
   about a touchpad that decides whether this driver can read it, and the
   machines it matters on have no serial port. */
static void log_report_descriptor(uint32_t length) {
    static const char digits[] = "0123456789abcdef";
    kernel_log_puts("[i2c-hid] report descriptor, ");
    kernel_log_put_dec(length);
    kernel_log_puts(" bytes:\n");
    char line[3 * 32 + 16];
    for (uint32_t at = 0; at < length; at += 32) {
        uint32_t n = 0;
        const char prefix[] = "[i2c-hid]  ";
        for (uint32_t k = 0; prefix[k]; k++) {
            line[n++] = prefix[k];
        }
        for (uint32_t i = at; i < length && i < at + 32; i++) {
            line[n++] = digits[report_descriptor[i] >> 4];
            line[n++] = digits[report_descriptor[i] & 0x0F];
            line[n++] = ' ';
        }
        line[n++] = '\n';
        line[n] = '\0';
        kernel_log_puts(line);
    }
}

static void log_touchpad_layout(void) {
    kernel_log_puts("[i2c-hid] precision touchpad: report id ");
    kernel_log_put_dec(touchpad_layout.report_id);
    kernel_log_puts(", ");
    kernel_log_put_dec(touchpad_layout.finger_count);
    kernel_log_puts(" contact(s) a report, X 0..");
    kernel_log_put_dec((uint32_t)touchpad_layout.x_maximum);
    kernel_log_puts(" over ");
    kernel_log_put_dec(touchpad_layout.width_tenths_mm);
    kernel_log_puts(" tenths of a mm, Y 0..");
    kernel_log_put_dec((uint32_t)touchpad_layout.y_maximum);
    kernel_log_puts(" over ");
    kernel_log_put_dec(touchpad_layout.height_tenths_mm);
    kernel_log_puts(", contact count ");
    kernel_log_puts(touchpad_layout.contact_count.present ? "yes" : "no");
    kernel_log_puts(", button ");
    kernel_log_puts(touchpad_layout.button.present ? "yes" : "no");
    kernel_log_puts(", input mode in feature report ");
    kernel_log_put_dec(touchpad_layout.input_mode_report_id);
    kernel_log_puts(".\n");
}

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
    out->report_id = precision ? touchpad_layout.report_id : layout.report_id;
    out->precision = (uint8_t)precision;
}

int i2c_touchpad_identity(uint16_t *vendor, uint16_t *product, int *is_precision) {
    if (!present) {
        return 0;
    }
    *vendor = descriptor.vendor_id;
    *product = descriptor.product_id;
    *is_precision = precision;
    return 1;
}

static int set_feature(uint8_t report_id, const uint8_t *payload, uint32_t length) {
    uint8_t command[64];
    uint32_t command_length = i2c_hid_build_set_feature(descriptor.command_register, descriptor.data_register,
                                                        report_id, payload, length, command, sizeof(command));
    if (command_length == 0) {
        return 0;
    }
    return designware_i2c_transfer(found_controller, found_address, command, command_length, 0, 0);
}

static int set_input_mode(uint8_t mode) {
    uint8_t payload[32];
    uint32_t length = hid_touchpad_build_input_mode(&touchpad_layout, mode, payload, sizeof(payload));
    if (length == 0 || !set_feature(touchpad_layout.input_mode_report_id, payload, length)) {
        return 0;
    }
    length = hid_touchpad_build_switches(&touchpad_layout, payload, sizeof(payload));
    if (length != 0) {
        set_feature(touchpad_layout.switches_report_id, payload, length);
    }
    return 1;
}

static void deliver_precision(const uint8_t *report, uint32_t length) {
    hid_touchpad_report_t decoded;
    if (!hid_touchpad_decode(&touchpad_layout, report, length, &decoded)) {
        statistics.wrong_report++;
        undecodable_in_a_row++;
        return;
    }
    undecodable_in_a_row = 0;
    statistics.reports++;
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    int count = touchpad_gestures_report(&gestures, &decoded, (uint32_t)clock_monotonic_ms(), events);
    for (int i = 0; i < count; i++) {
        statistics.injected++;
        mouse_inject_from(MOUSE_SOURCE_TRACKPAD, events[i].tap ? MOUSE_FLAG_TAP : 0, events[i].dx, events[i].dy,
                          events[i].buttons, events[i].wheel);
    }
    if (count > 0) {
        last_buttons = events[count - 1].buttons;
    }
}

/* A plain read, with no register written first. That is how HID over I2C
   hands over an input report and what Linux's i2c-hid does; writing the input
   register's address and then reading is a different transaction, and the
   first real touchpad this ran against answered it with nothing usable. */
static int poll_once(void) {
    uint32_t want = descriptor.max_input_length;
    if (want > sizeof(input_buffer)) {
        want = sizeof(input_buffer);
    }
    statistics.polls++;
    if (!designware_i2c_transfer(found_controller, found_address, 0, 0, input_buffer, want)) {
        statistics.read_failures++;
        return 0;
    }

    uint16_t length = i2c_hid_input_length(input_buffer, want);
    statistics.last_length = length;
    for (uint32_t i = 0; i < sizeof(statistics.last_bytes); i++) {
        statistics.last_bytes[i] = i < want ? input_buffer[i] : 0;
    }
    if (length <= 2 || length > want) {
        statistics.empty++;
        return 0;
    }

    if (precision && input_buffer[2] == touchpad_layout.report_id) {
        uint32_t before = statistics.injected;
        deliver_precision(input_buffer + 2, (uint32_t)(length - 2));
        return statistics.injected != before || gestures.touching;
    }

    hid_mouse_report_t report;
    if (!have_mouse_layout || !hid_mouse_decode(&layout, input_buffer + 2, (uint32_t)(length - 2), &report)) {
        statistics.wrong_report++;
        if (precision && ++undecodable_in_a_row >= UNDECODABLE_BEFORE_MOUSE_MODE && have_mouse_layout) {
            precision = 0;
            set_input_mode(HID_TOUCHPAD_INPUT_MODE_MOUSE);
            kernel_log_puts("[i2c-hid] the pad's reports in touchpad mode could not be read - put it back "
                            "in its mouse mode.\n");
        }
        return 0;
    }
    undecodable_in_a_row = 0;
    statistics.reports++;

    if (report.dx == 0 && report.dy == 0 && report.wheel == 0 && report.buttons == last_buttons) {
        return 0;
    }
    last_buttons = report.buttons;
    statistics.injected++;
    mouse_inject_from(MOUSE_SOURCE_TRACKPAD, 0, report.dx, report.dy, report.buttons, report.wheel);
    return 1;
}

/* M197. This used pit_sleep_ms, which halts with the task still RUNNING -
   M170's bug, in the one driver QEMU never exercises - and every poll is a
   whole report clocked over a 400 kHz bus by a busy-waiting transfer. On the
   laptop that was a core's steady share with nobody touching anything. A
   finger that is moving is read every tick; a pad left alone for half a
   second is read about forty times a second, and the first report brings the
   full rate back. */
static void poll_task(void *argument) {
    (void)argument;
    designware_i2c_sleep_while_clocking(1);
    uint64_t last_report_ms = 0;
    for (;;) {
        uint64_t now_ms = clock_monotonic_ms();
        if (poll_once() || last_buttons != 0) {
            last_report_ms = now_ms;
        }
        scheduler_sleep_ms(now_ms - last_report_ms < RESTING_AFTER_MS ? POLL_INTERVAL_MS
                                                                      : RESTING_POLL_INTERVAL_MS);
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

    /* M200: where touchpads live, first. The sweep asks every address from
       0x08 up and an address nobody answers costs tens of milliseconds, so
       the laptop's Elan pad at 0x15 was found after fourteen of them - 0.6 s
       of every boot. Elan and Synaptics parts are at 0x15 and 0x2C on almost
       every machine; the sweep is still there for the ones that are not. */
    static const uint8_t likely[] = {0x15, 0x2C, 0x10, 0x14, 0x20, 0x2A, 0x38, 0x5D};
    const int likely_count = (int)(sizeof(likely) / sizeof(likely[0]));
    for (int controller = 0; controller < controllers && found_controller < 0; controller++) {
        for (int step = 0; step < likely_count + (LAST_ADDRESS - FIRST_ADDRESS + 1); step++) {
            uint8_t address;
            if (step < likely_count) {
                address = likely[step];
            } else {
                address = (uint8_t)(FIRST_ADDRESS + (step - likely_count));
                int tried = 0;
                for (int k = 0; k < likely_count; k++) {
                    tried |= likely[k] == address;
                }
                if (tried) {
                    continue;
                }
            }
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

    log_report_descriptor(want);

    have_mouse_layout = hid_report_find_mouse(report_descriptor, want, &layout);
    precision = 0;
    if (hid_report_find_touchpad(report_descriptor, want, &touchpad_layout)) {
        log_touchpad_layout();
        if (touchpad_layout.input_mode.present && set_input_mode(HID_TOUCHPAD_INPUT_MODE_TOUCHPAD)) {
            precision = 1;
            touchpad_gestures_init(&gestures, &touchpad_layout);
            kernel_log_puts("[i2c-hid] switched to touchpad mode: two fingers scroll, a tap clicks.\n");
        } else {
            kernel_log_puts(touchpad_layout.input_mode.present
                                ? "[i2c-hid] the pad refused the switch to touchpad mode - staying a mouse.\n"
                                : "[i2c-hid] the touchpad collection has no input-mode feature to switch "
                                  "with - staying a mouse.\n");
        }
    }
    if (!have_mouse_layout && !precision) {
        kernel_log_puts("[i2c-hid] the report descriptor has no mouse collection in it and no touchpad "
                   "this driver can switch to - this device is something other than a pointing "
                   "device.\n");
        return 0;
    }
    if (!have_mouse_layout) {
        present = 1;
        return 1;
    }

    kernel_log_puts("[i2c-hid] mouse collection: report id ");
    kernel_log_put_dec(layout.report_id);
    kernel_log_puts(", ");
    kernel_log_put_dec(layout.button_count);
    kernel_log_puts(" button(s), ");
    kernel_log_put_dec(layout.report_bits);
    kernel_log_puts(" bits, wheel ");
    kernel_log_puts(layout.has_wheel ? "yes" : "no");
    kernel_log_puts(", every poll reads ");
    kernel_log_put_dec(descriptor.max_input_length);
    kernel_log_puts(" bytes.\n");

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
