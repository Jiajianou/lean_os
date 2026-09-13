#include "rtl8139_ring.h"

#include "rtl8139.h"

uint32_t rtl8139_ring_take(const uint8_t *ring, uint32_t offset, rtl8139_rx_t *out) {
    const uint8_t *header = ring + offset;
    uint16_t status = (uint16_t)(header[0] | (header[1] << 8));
    uint16_t packet_len = (uint16_t)(header[2] | (header[3] << 8));
    uint16_t frame_len = (uint16_t)(packet_len >= 4 ? packet_len - 4 : 0);

    out->status = status;
    out->frame = header + 4;
    out->len = 0;
    if ((status & RTL8139_RX_ROK) && frame_len > 0 && frame_len <= RTL8139_MAX_FRAME) {
        out->len = frame_len;
    }

    return ((offset + 4u + packet_len + 3u) & ~3u) % RTL8139_RING_LEN;
}
