#pragma once

#include <stdint.h>

#define PS2_KEYBOARD_PORT_PRESENT (1u << 0)
#define PS2_AUX_PORT_PRESENT      (1u << 1)
#define PS2_CONTROLLER_PRESENT    (1u << 2)

uint32_t ps2_controller_init(void);

uint32_t ps2_controller_ports(void);

void ps2_controller_write_command(uint8_t command);

void ps2_controller_write_data(uint8_t data);

int ps2_controller_read_data(uint8_t *out);

void ps2_controller_flush(void);

int ps2_controller_write_aux(uint8_t data);
