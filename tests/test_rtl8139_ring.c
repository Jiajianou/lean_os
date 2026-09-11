/* tests/test_rtl8139_ring.c - M116
 *
 * The RTL8139 receive ring, written the way the card writes it and read
 * back through the driver's own code.
 *
 * The writer below is not this project's idea of the layout. It is
 * QEMU's rtl8139_write_buffer() and the ring-mode half of
 * rtl8139_do_receive() from hw/net/rtl8139.c, transcribed: a 4-byte
 * header, the frame, a 4-byte CRC, and then the write address rounded up
 * to four bytes and wrapped. The one line of it that matters is the
 * branch on RCR_WRAP - with it set (and rtl8139.c sets it) a packet that
 * runs past the end of the ring is written *contiguously into the pad*,
 * not split back to offset 0.
 *
 * The driver had that backwards from M27 to M116. Every frame that
 * straddled the end came back with its tail copied from the start of the
 * ring - an older packet - so TCP's checksum rejected it, silently, and
 * the peer's retransmission timer paid for it: a full-sized segment lands
 * on the wrap roughly one time in five, and each one cost 1.5 s. Every
 * test in this tree used loopback or packets small enough never to
 * straddle, which is why it lived for ninety milestones. The first test
 * here fails against that code on its first wrap.
 *
 * The ring is malloc'd at exactly RTL8139_RING_LEN + RTL8139_RING_PAD
 * bytes, so ASan turns a reader - or a pad - that is one byte short into
 * a failure rather than a read of whatever lies beyond. */
#include "check.h"

#include "drivers/rtl8139.h"
#include "drivers/rtl8139_ring.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- the card, as QEMU models it -------------------------------------- */

typedef struct {
    uint8_t *ring;      /* RTL8139_RING_LEN + RTL8139_RING_PAD bytes */
    uint32_t addr;      /* RxBufAddr: where the next packet's header goes */
} nic_t;

/* rtl8139_write_buffer() with RCR_WRAP set and an 8 KiB ring: the
 * "non-wrapping path or overwrapping enabled" branch, always. */
static void nic_write(nic_t *n, const void *buf, uint32_t size) {
    REQUIRE(n->addr + size <= RTL8139_RING_LEN + RTL8139_RING_PAD);
    memcpy(n->ring + n->addr, buf, size);
    n->addr += size;
}

static void nic_receive(nic_t *n, const uint8_t *frame, uint32_t size, uint16_t status) {
    uint32_t header = (uint32_t)status | ((size + 4u) << 16);
    uint8_t h[4] = {(uint8_t)header, (uint8_t)(header >> 8),
                    (uint8_t)(header >> 16), (uint8_t)(header >> 24)};
    uint8_t crc[4] = {0xC0, 0xFF, 0xEE, 0x00}; /* the value is never read */
    nic_write(n, h, 4);
    nic_write(n, frame, size);
    nic_write(n, crc, 4);
    n->addr = ((n->addr + 3u) & ~3u) % RTL8139_RING_LEN; /* MOD2(RX_ALIGN(addr), size) */
}

static nic_t nic_new(void) {
    nic_t n;
    n.ring = malloc(RTL8139_RING_LEN + RTL8139_RING_PAD);
    memset(n.ring, 0xA5, RTL8139_RING_LEN + RTL8139_RING_PAD);
    n.addr = 0;
    return n;
}

/* A frame whose every byte says which frame it is and where in it, so a
 * tail copied from somewhere else cannot match by accident. */
static void fill_frame(uint8_t *f, uint32_t len, uint32_t serial) {
    for (uint32_t i = 0; i < len; i++) {
        f[i] = (uint8_t)(serial * 131u + i * 7u + (i >> 8));
    }
}

/* ---- the tests ----------------------------------------------------------- */

/* The case that was broken, at the size that broke it: 1494-byte frames
 * are what a 1440-byte TCP segment from QEMU's SLIRP arrives as, and they
 * are what the packet capture in M116 shows being thrown away. Three
 * hundred of them wrap the ring about fifty-five times, at every
 * alignment the stride produces. */
TEST(rtl8139_ring, full_sized_frames_survive_every_wrap) {
    nic_t n = nic_new();
    uint32_t reader = 0;
    uint8_t frame[RTL8139_MAX_FRAME], got[RTL8139_MAX_FRAME];
    int straddled = 0;

    for (uint32_t k = 0; k < 300; k++) {
        uint32_t len = 1494;
        fill_frame(frame, len, k);
        REQUIRE(n.addr == reader);
        if (reader + 4 + len > RTL8139_RING_LEN) {
            straddled++;
        }
        nic_receive(&n, frame, len, RTL8139_RX_ROK);

        rtl8139_rx_t rx;
        reader = rtl8139_ring_take(n.ring, reader, &rx);
        CHECK_EQ(rx.len, len);
        memcpy(got, rx.frame, rx.len);
        CHECK_MEMEQ(got, frame, len);
        CHECK_EQ(reader, n.addr);
    }
    /* Not vacuous: the stride has to have put frames across the end. */
    CHECK(straddled > 40);
    free(n.ring);
}

/* Every legal frame length, so the rounding is exercised at all four
 * residues and the pad at every depth it can reach. */
TEST(rtl8139_ring, every_frame_length) {
    nic_t n = nic_new();
    uint32_t reader = 0;
    uint8_t frame[RTL8139_MAX_FRAME];
    for (uint32_t len = 14; len <= RTL8139_MAX_FRAME; len++) {
        fill_frame(frame, len, len);
        nic_receive(&n, frame, len, RTL8139_RX_ROK);
        rtl8139_rx_t rx;
        reader = rtl8139_ring_take(n.ring, reader, &rx);
        REQUIRE(rx.len == len);
        CHECK(memcmp(rx.frame, frame, len) == 0);
        CHECK_EQ(reader, n.addr);
        CHECK_EQ(reader % 4, 0);
    }
    free(n.ring);
}

/* The deepest the pad is ever used: a header in the ring's last four
 * bytes, followed by the largest frame there is. The ring is allocated
 * at exactly its declared size, so a pad that is short is an ASan report
 * here rather than a corrupted neighbour on the machine. */
TEST(rtl8139_ring, the_deepest_straddle_fits_the_pad) {
    nic_t n = nic_new();
    uint8_t frame[RTL8139_MAX_FRAME];
    n.addr = RTL8139_RING_LEN - 4;
    fill_frame(frame, RTL8139_MAX_FRAME, 99);
    nic_receive(&n, frame, RTL8139_MAX_FRAME, RTL8139_RX_ROK);

    rtl8139_rx_t rx;
    uint32_t next = rtl8139_ring_take(n.ring, RTL8139_RING_LEN - 4, &rx);
    REQUIRE(rx.len == RTL8139_MAX_FRAME);
    CHECK(memcmp(rx.frame, frame, RTL8139_MAX_FRAME) == 0);
    CHECK_EQ(next, n.addr);
    CHECK(next < RTL8139_RING_LEN);
    free(n.ring);
}

/* A header the card marked bad is skipped - nothing is delivered - but
 * the reader still moves past it by the length it claims, which is where
 * the card put the next packet. */
TEST(rtl8139_ring, a_bad_packet_is_skipped_and_the_next_one_is_found) {
    nic_t n = nic_new();
    uint8_t bad[100], good[200];
    fill_frame(bad, sizeof(bad), 1);
    fill_frame(good, sizeof(good), 2);
    nic_receive(&n, bad, sizeof(bad), 0 /* no ROK */);
    nic_receive(&n, good, sizeof(good), RTL8139_RX_ROK);

    rtl8139_rx_t rx;
    uint32_t off = rtl8139_ring_take(n.ring, 0, &rx);
    CHECK_EQ(rx.len, 0);
    off = rtl8139_ring_take(n.ring, off, &rx);
    REQUIRE(rx.len == sizeof(good));
    CHECK(memcmp(rx.frame, good, sizeof(good)) == 0);
    CHECK_EQ(off, n.addr);
    free(n.ring);
}

/* A length no Ethernet frame can have - the card would not write one,
 * but a ring read at the wrong offset looks exactly like this - is not
 * handed to the stack. */
TEST(rtl8139_ring, an_impossible_length_is_not_delivered) {
    nic_t n = nic_new();
    uint8_t h[4] = {RTL8139_RX_ROK, 0, 0xFF, 0x0F}; /* 4095 bytes */
    memcpy(n.ring, h, 4);
    rtl8139_rx_t rx;
    rtl8139_ring_take(n.ring, 0, &rx);
    CHECK_EQ(rx.len, 0);

    uint8_t tiny[4] = {RTL8139_RX_ROK, 0, 4, 0}; /* a CRC and no frame */
    memcpy(n.ring, tiny, 4);
    rtl8139_ring_take(n.ring, 0, &rx);
    CHECK_EQ(rx.len, 0);
    free(n.ring);
}
