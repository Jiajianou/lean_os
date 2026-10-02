#pragma once

int i2c_touchpad_init(void);

int i2c_touchpad_present(void);

void i2c_touchpad_start(void);

#include <stdint.h>

typedef struct {
    uint32_t polls;
    uint32_t read_failures;
    uint32_t empty;
    uint32_t wrong_report;
    uint32_t reports;
    uint32_t injected;
    uint16_t last_length;
    uint8_t last_bytes[8];
    uint8_t report_id;
    uint8_t precision;
} i2c_touchpad_statistics_t;

void i2c_touchpad_statistics(i2c_touchpad_statistics_t *out);

int i2c_touchpad_identity(uint16_t *vendor, uint16_t *product, int *is_precision);
