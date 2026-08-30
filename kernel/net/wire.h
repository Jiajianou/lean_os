/* kernel/net/wire.h
 *
 * The wire format: big-endian field accessors and the internet checksum.
 *
 * Every protocol in this directory reads and writes the same two things -
 * multi-byte integers in network byte order, and RFC 1071's one's
 * complement sum - and every one of them had grown its own private copy.
 * By M79 that was eighteen `static` definitions across six files: four
 * each of read_be32 and write_be32, two each of read_be16, write_be16,
 * sum16 and checksum16, and two more of the checksum fold under two
 * different names (`fold` in tcp.c, `fold_checksum` in udp.c). Every one
 * of them byte for byte identical to its siblings. Nothing had gone
 * wrong yet, which is exactly when this is cheap to fix: eighteen copies
 * of a definition are eighteen places a fix has to land, and the fourth
 * copy is written by pasting the third.
 *
 * These are deliberately byte-at-a-time rather than a load-and-swap.
 * This kernel only ever runs little-endian, so a swap would work - but a
 * byte-at-a-time accessor needs no alignment from the caller, and these
 * are pointed straight at unaligned offsets inside received frames
 * (a TCP header sitting at whatever offset the IP options left it at).
 * The compiler folds the shifts back into a movbe/bswap where it can.
 *
 * The checksum is split in two on purpose. `net_sum16` accumulates into
 * a caller-held sum so that TCP and UDP can run the pseudo-header, the
 * real header and the payload through it in three calls without
 * assembling a buffer; `net_fold16` is the fold-and-complement that
 * turns any such accumulation into the field that goes on the wire.
 * `net_checksum16` is the one-shot case (IP, ICMP) spelled in terms of
 * both, so there is exactly one copy of the arithmetic.
 */
#pragma once

#include <stdint.h>

static inline uint16_t net_read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t net_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void net_write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void net_write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Adds `len` bytes to a running one's complement sum, big-endian pairs,
 * odd tail byte padded on the right as RFC 1071 requires. The sum is
 * carried in 32 bits and folded only at the end - which is what lets a
 * caller run several regions through it in sequence. */
static inline uint32_t net_sum16(uint32_t sum, const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    }
    if (len & 1) {
        sum += (uint32_t)data[len - 1] << 8;
    }
    return sum;
}

/* Folds the carries out of an accumulated sum and complements it: the
 * value that actually goes in a checksum field. */
static inline uint16_t net_fold16(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static inline uint16_t net_checksum16(const uint8_t *data, uint16_t len) {
    return net_fold16(net_sum16(0, data, len));
}
