#include "rtl8139_ring.h"

#include "rtl8139.h" /* RTL8139_MAX_FRAME */

uint32_t rtl8139_ring_take(const uint8_t *ring, uint32_t offset, rtl8139_rx_t *out) {
    const uint8_t *header = ring + offset;
    uint16_t status = (uint16_t)(header[0] | (header[1] << 8));
    uint16_t packet_len = (uint16_t)(header[2] | (header[3] << 8));
    uint16_t frame_len = (uint16_t)(packet_len >= 4 ? packet_len - 4 : 0);

    out->status = status;
    out->frame = header + 4;
    out->len = 0;
    /* No reassembly, and that is the fix. Until M116 a frame that ran
     * past RTL8139_RING_LEN was stitched back together from the tail of
     * the ring and its *start* - the layout a card with RCR_WRAP clear
     * would have written. This one has it set, so the bytes at the start
     * of the ring were an older packet, and one full-sized frame in five
     * reached TCP with a corrupt tail. See this file's header. */
    if ((status & RTL8139_RX_ROK) && frame_len > 0 && frame_len <= RTL8139_MAX_FRAME) {
        out->len = frame_len;
    }

    /* Past this packet's header, frame and CRC, rounded up to the 4-byte
     * boundary the card starts the next one on, and wrapped. */
    return ((offset + 4u + packet_len + 3u) & ~3u) % RTL8139_RING_LEN;
}
