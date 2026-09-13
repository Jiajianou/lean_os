#include "check.h"

#include "drivers/rtl8139.h"
#include "drivers/rtl8139_ring.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *ring;
    uint32_t address;
} nic_t;

static void nic_write(nic_t *n, const void *buffer, uint32_t size) {
    REQUIRE(n->address + size <= RTL8139_RING_LENGTH + RTL8139_RING_PAD);
    memcpy(n->ring + n->address, buffer, size);
    n->address += size;
}

static void nic_receive(nic_t *n, const uint8_t *frame, uint32_t size, uint16_t status) {
    uint32_t header = (uint32_t)status | ((size + 4u) << 16);
    uint8_t h[4] = {(uint8_t)header, (uint8_t)(header >> 8),
                    (uint8_t)(header >> 16), (uint8_t)(header >> 24)};
    uint8_t crc[4] = {0xC0, 0xFF, 0xEE, 0x00};
    nic_write(n, h, 4);
    nic_write(n, frame, size);
    nic_write(n, crc, 4);
    n->address = ((n->address + 3u) & ~3u) % RTL8139_RING_LENGTH;
}

static nic_t nic_new(void) {
    nic_t n;
    n.ring = malloc(RTL8139_RING_LENGTH + RTL8139_RING_PAD);
    memset(n.ring, 0xA5, RTL8139_RING_LENGTH + RTL8139_RING_PAD);
    n.address = 0;
    return n;
}

static void fill_frame(uint8_t *f, uint32_t length, uint32_t serial) {
    for (uint32_t i = 0; i < length; i++) {
        f[i] = (uint8_t)(serial * 131u + i * 7u + (i >> 8));
    }
}

TEST(rtl8139_ring, full_sized_frames_survive_every_wrap) {
    nic_t n = nic_new();
    uint32_t reader = 0;
    uint8_t frame[RTL8139_MAX_FRAME], got[RTL8139_MAX_FRAME];
    int straddled = 0;

    for (uint32_t k = 0; k < 300; k++) {
        uint32_t length = 1494;
        fill_frame(frame, length, k);
        REQUIRE(n.address == reader);
        if (reader + 4 + length > RTL8139_RING_LENGTH) {
            straddled++;
        }
        nic_receive(&n, frame, length, RTL8139_RX_ROK);

        rtl8139_rx_t rx;
        reader = rtl8139_ring_take(n.ring, reader, &rx);
        CHECK_EQ(rx.length, length);
        memcpy(got, rx.frame, rx.length);
        CHECK_MEMEQ(got, frame, length);
        CHECK_EQ(reader, n.address);
    }
    CHECK(straddled > 40);
    free(n.ring);
}

TEST(rtl8139_ring, every_frame_length) {
    nic_t n = nic_new();
    uint32_t reader = 0;
    uint8_t frame[RTL8139_MAX_FRAME];
    for (uint32_t length = 14; length <= RTL8139_MAX_FRAME; length++) {
        fill_frame(frame, length, length);
        nic_receive(&n, frame, length, RTL8139_RX_ROK);
        rtl8139_rx_t rx;
        reader = rtl8139_ring_take(n.ring, reader, &rx);
        REQUIRE(rx.length == length);
        CHECK(memcmp(rx.frame, frame, length) == 0);
        CHECK_EQ(reader, n.address);
        CHECK_EQ(reader % 4, 0);
    }
    free(n.ring);
}

TEST(rtl8139_ring, the_deepest_straddle_fits_the_pad) {
    nic_t n = nic_new();
    uint8_t frame[RTL8139_MAX_FRAME];
    n.address = RTL8139_RING_LENGTH - 4;
    fill_frame(frame, RTL8139_MAX_FRAME, 99);
    nic_receive(&n, frame, RTL8139_MAX_FRAME, RTL8139_RX_ROK);

    rtl8139_rx_t rx;
    uint32_t next = rtl8139_ring_take(n.ring, RTL8139_RING_LENGTH - 4, &rx);
    REQUIRE(rx.length == RTL8139_MAX_FRAME);
    CHECK(memcmp(rx.frame, frame, RTL8139_MAX_FRAME) == 0);
    CHECK_EQ(next, n.address);
    CHECK(next < RTL8139_RING_LENGTH);
    free(n.ring);
}

TEST(rtl8139_ring, a_bad_packet_is_skipped_and_the_next_one_is_found) {
    nic_t n = nic_new();
    uint8_t bad[100], good[200];
    fill_frame(bad, sizeof(bad), 1);
    fill_frame(good, sizeof(good), 2);
    nic_receive(&n, bad, sizeof(bad), 0  );
    nic_receive(&n, good, sizeof(good), RTL8139_RX_ROK);

    rtl8139_rx_t rx;
    uint32_t off = rtl8139_ring_take(n.ring, 0, &rx);
    CHECK_EQ(rx.length, 0);
    off = rtl8139_ring_take(n.ring, off, &rx);
    REQUIRE(rx.length == sizeof(good));
    CHECK(memcmp(rx.frame, good, sizeof(good)) == 0);
    CHECK_EQ(off, n.address);
    free(n.ring);
}

TEST(rtl8139_ring, an_impossible_length_is_not_delivered) {
    nic_t n = nic_new();
    uint8_t h[4] = {RTL8139_RX_ROK, 0, 0xFF, 0x0F};
    memcpy(n.ring, h, 4);
    rtl8139_rx_t rx;
    rtl8139_ring_take(n.ring, 0, &rx);
    CHECK_EQ(rx.length, 0);

    uint8_t tiny[4] = {RTL8139_RX_ROK, 0, 4, 0};
    memcpy(n.ring, tiny, 4);
    rtl8139_ring_take(n.ring, 0, &rx);
    CHECK_EQ(rx.length, 0);
    free(n.ring);
}
