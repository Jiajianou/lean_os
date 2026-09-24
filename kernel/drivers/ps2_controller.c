#include "ps2_controller.h"

#include "architecture/x86_64/io.h"
#include "drivers/kernel_log.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_INPUT_FULL  0x02u

#define PS2_COMMAND_READ_CONFIG     0x20
#define PS2_COMMAND_WRITE_CONFIG    0x60
#define PS2_COMMAND_DISABLE_AUX     0xA7
#define PS2_COMMAND_ENABLE_AUX      0xA8
#define PS2_COMMAND_TEST_AUX        0xA9
#define PS2_COMMAND_SELF_TEST       0xAA
#define PS2_COMMAND_TEST_KEYBOARD   0xAB
#define PS2_COMMAND_DISABLE_KEYBOARD 0xAD
#define PS2_COMMAND_ENABLE_KEYBOARD  0xAE
#define PS2_COMMAND_WRITE_AUX        0xD4

#define PS2_CONFIG_KEYBOARD_INTERRUPT 0x01u
#define PS2_CONFIG_AUX_INTERRUPT      0x02u
#define PS2_CONFIG_KEYBOARD_DISABLED  0x10u
#define PS2_CONFIG_AUX_DISABLED       0x20u
#define PS2_CONFIG_TRANSLATION        0x40u

#define PS2_SELF_TEST_PASSED 0x55
#define PS2_PORT_TEST_PASSED 0x00
#define PS2_DEVICE_ACKNOWLEDGE 0xFA
#define PS2_KEYBOARD_ENABLE_SCANNING 0xF4

#define PS2_POLL_LIMIT 200000

static uint32_t ports;

static int wait_input_clear(void) {
    for (int timeout = PS2_POLL_LIMIT; timeout > 0; timeout--) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
            return 1;
        }
    }
    return 0;
}

void ps2_controller_write_command(uint8_t command) {
    wait_input_clear();
    outb(PS2_COMMAND_PORT, command);
}

void ps2_controller_write_data(uint8_t data) {
    wait_input_clear();
    outb(PS2_DATA_PORT, data);
}

int ps2_controller_read_data(uint8_t *out) {
    for (int timeout = PS2_POLL_LIMIT; timeout > 0; timeout--) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            *out = inb(PS2_DATA_PORT);
            return 1;
        }
    }
    return 0;
}

void ps2_controller_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
            return;
        }
        (void)inb(PS2_DATA_PORT);
    }
}

int ps2_controller_write_aux(uint8_t data) {
    ps2_controller_write_command(PS2_COMMAND_WRITE_AUX);
    ps2_controller_write_data(data);
    uint8_t response = 0;
    if (!ps2_controller_read_data(&response)) {
        return 0;
    }
    return response == PS2_DEVICE_ACKNOWLEDGE;
}

uint32_t ps2_controller_ports(void) {
    return ports;
}

uint32_t ps2_controller_init(void) {
    ports = 0;

    if (inb(PS2_STATUS_PORT) == 0xFF) {
        kernel_log_puts("[ps2] status port reads 0xFF - no 8042 on this machine.\n");
        return 0;
    }
    ports |= PS2_CONTROLLER_PRESENT;

    ps2_controller_write_command(PS2_COMMAND_DISABLE_KEYBOARD);
    ps2_controller_write_command(PS2_COMMAND_DISABLE_AUX);
    ps2_controller_flush();

    ps2_controller_write_command(PS2_COMMAND_READ_CONFIG);
    uint8_t config = 0;
    if (!ps2_controller_read_data(&config)) {
        kernel_log_puts("[ps2] the controller did not answer a config byte read - "
                   "leaving it as the firmware set it up.\n");
        ports |= PS2_KEYBOARD_PORT_PRESENT;
        ps2_controller_write_command(PS2_COMMAND_ENABLE_KEYBOARD);
        ps2_controller_write_command(PS2_COMMAND_ENABLE_AUX);
        return ports;
    }

    config &= (uint8_t)~(PS2_CONFIG_KEYBOARD_DISABLED | PS2_CONFIG_AUX_DISABLED);
    config |= PS2_CONFIG_KEYBOARD_INTERRUPT | PS2_CONFIG_AUX_INTERRUPT | PS2_CONFIG_TRANSLATION;
    ps2_controller_write_command(PS2_COMMAND_WRITE_CONFIG);
    ps2_controller_write_data(config);

    ps2_controller_write_command(PS2_COMMAND_TEST_KEYBOARD);
    uint8_t result = 0xFF;
    if (ps2_controller_read_data(&result) && result == PS2_PORT_TEST_PASSED) {
        ports |= PS2_KEYBOARD_PORT_PRESENT;
    }

    ps2_controller_write_command(PS2_COMMAND_TEST_AUX);
    result = 0xFF;
    if (ps2_controller_read_data(&result) && result == PS2_PORT_TEST_PASSED) {
        ports |= PS2_AUX_PORT_PRESENT;
    }

    ps2_controller_write_command(PS2_COMMAND_ENABLE_KEYBOARD);
    ps2_controller_write_command(PS2_COMMAND_ENABLE_AUX);

    if (ports & PS2_KEYBOARD_PORT_PRESENT) {
        ps2_controller_write_data(PS2_KEYBOARD_ENABLE_SCANNING);
        uint8_t acknowledge = 0;
        if (!ps2_controller_read_data(&acknowledge) || acknowledge != PS2_DEVICE_ACKNOWLEDGE) {
            kernel_log_puts("[ps2] the keyboard did not acknowledge 0xF4 - it was already "
                       "scanning if the firmware left it that way.\n");
        }
    }
    ps2_controller_flush();

    kernel_log_puts("[ps2] 8042 present, config 0x");
    kernel_log_put_hex32(config);
    kernel_log_puts(", keyboard port ");
    kernel_log_puts((ports & PS2_KEYBOARD_PORT_PRESENT) ? "ok" : "absent");
    kernel_log_puts(", auxiliary port ");
    kernel_log_puts((ports & PS2_AUX_PORT_PRESENT) ? "ok" : "absent");
    kernel_log_puts(".\n");

    return ports;
}
