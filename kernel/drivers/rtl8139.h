#pragma once

#include <stdint.h>

int rtl8139_init(void);

const uint8_t *rtl8139_mac(void);

#define RTL8139_MAX_FRAME 1514
int rtl8139_send(const uint8_t *frame, uint16_t len);

uint32_t rtl8139_tx_error_count(void);
