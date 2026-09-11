/* kernel/drivers/rtl8139_ring.h - M116
 *
 * The RTL8139's receive ring, read without the hardware.
 *
 * Split out of rtl8139.c so the one piece of that driver whose failure
 * mode is a *plausible wrong answer* can be tested off the machine. The
 * rest of the driver is port I/O, and a port that does the wrong thing
 * fails loudly; this is arithmetic over a buffer, and when it was wrong
 * it handed the stack a well-formed Ethernet frame with the wrong bytes
 * in its tail. TCP's checksum then threw the segment away without a
 * word, the peer retransmitted it 1.5 s later, and the only visible
 * symptom anywhere was that every web page loaded at 0.4 KiB/s. See
 * M116 in milestones.md for the packet capture that found it.
 *
 * ---- the layout, which is the whole contract -------------------------
 *
 * The card DMAs into one physically contiguous ring of RTL8139_RING_LEN
 * bytes. Each packet is a 4-byte header - status (le16), then length
 * (le16, which counts the 4-byte CRC the card appends) - followed by the
 * frame and the CRC, and the next packet starts at the next 4-byte
 * boundary, modulo the ring length.
 *
 * rtl8139.c sets RCR_WRAP, and that bit decides how a packet that runs
 * past the end of the ring is laid out: with it set, the card does NOT
 * wrap the packet to the start of the ring - it keeps writing past the
 * end, into RTL8139_RING_PAD bytes of slack the driver allocates for
 * exactly that. So a frame is ALWAYS contiguous in memory, and only the
 * *next* packet's offset wraps. QEMU's hw/net/rtl8139.c says the same
 * thing in rtl8139_write_buffer ("non-wrapping path or overwrapping
 * enabled"), and the host test models that function rather than this
 * file's idea of it. */
#pragma once

#include <stdint.h>

#define RTL8139_RING_LEN 8192   /* RCR RBLEN = 00: an 8 KiB ring */
#define RTL8139_RING_PAD (16 + 1536) /* the header slack, and one whole frame past the end */

/* Receive status bits in a packet header (RTL8139 datasheet, "Receive
 * Status Register in Rx Packet Header"). ROK is the only one that says
 * the frame is usable. */
#define RTL8139_RX_ROK 0x0001

typedef struct {
    const uint8_t *frame;   /* the Ethernet frame, contiguous, CRC excluded */
    uint16_t len;           /* its length; 0 means "nothing usable here" */
    uint16_t status;        /* the card's header status word */
} rtl8139_rx_t;

/* Decodes the packet whose header is at `offset` in `ring` (which must
 * be RTL8139_RING_LEN + RTL8139_RING_PAD bytes) into `out`, and returns
 * the offset of the next packet's header.
 *
 * A packet the card marked bad, or one whose length cannot be an
 * Ethernet frame, comes back with out->len == 0 and is to be skipped;
 * the returned offset still moves past it by the length the header
 * claims, which is what the card did when it wrote the next one. */
uint32_t rtl8139_ring_take(const uint8_t *ring, uint32_t offset, rtl8139_rx_t *out);
