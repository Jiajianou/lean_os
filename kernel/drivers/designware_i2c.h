#pragma once

#include <stdint.h>

#define DESIGNWARE_I2C_MAX_CONTROLLERS 4

#define DESIGNWARE_I2C_COMPONENT_TYPE 0x44570140u

int designware_i2c_init(void);

int designware_i2c_controller_count(void);

int designware_i2c_transfer(int controller, uint8_t address, const uint8_t *write, uint32_t write_length,
                            uint8_t *read, uint32_t read_length);
