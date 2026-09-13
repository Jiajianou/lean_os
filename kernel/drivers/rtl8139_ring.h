#pragma once

#include <stdint.h>

#define RTL8139_RING_LENGTH 8192
#define RTL8139_RING_PAD (16 + 1536)

#define RTL8139_RX_ROK 0x0001

typedef struct {
    const uint8_t *frame;
    uint16_t length;
    uint16_t status;
} rtl8139_rx_t;

uint32_t rtl8139_ring_take(const uint8_t *ring, uint32_t offset, rtl8139_rx_t *out);
