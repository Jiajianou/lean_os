/* tests/fakes/fake_socket.c - Q4
 *
 * The layer above UDP and TCP, recording what was delivered to it.
 *
 * udp_handle_packet's whole job is to decide whether a datagram belongs
 * to a socket and hand it up. Faking the hand-up is what lets a test say
 * "this malformed datagram was dropped" and "that well-formed one arrived
 * with exactly these bytes" - which is the distinction that matters and
 * the one a crash-only fuzz check cannot make. */
/* The real header, included so the compiler checks this fake against the
 * declaration it is standing in for. Written without it first, and the
 * parameter order came out wrong - (src_ip, src_port, dst_port) rather
 * than (dst_port, src_ip, src_port) - and it linked silently, because
 * nothing cross-checks a definition against a prototype it never saw.
 * A fake that does not match is worse than no fake: every assertion built
 * on it is confidently wrong. */
#include "net/socket.h"

#include <stdint.h>
#include <string.h>

void panic(const char *msg);

#define MAX_DELIVERED 32
#define MAX_PAYLOAD 2048

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t len;
    uint8_t data[MAX_PAYLOAD];
} delivery_t;

static delivery_t delivered[MAX_DELIVERED];
static int delivered_count;

void fake_socket_reset(void);
int fake_socket_delivered_count(void);
const uint8_t *fake_socket_delivered(int i, uint32_t *len_out, uint16_t *dst_port_out);
uint16_t fake_socket_delivered_src_port(int i);
uint32_t fake_socket_delivered_src_ip(int i);

void fake_socket_reset(void) { delivered_count = 0; }
int fake_socket_delivered_count(void) { return delivered_count; }

const uint8_t *fake_socket_delivered(int i, uint32_t *len_out, uint16_t *dst_port_out) {
    if (i < 0 || i >= delivered_count) {
        return NULL;
    }
    if (len_out) { *len_out = delivered[i].len; }
    if (dst_port_out) { *dst_port_out = delivered[i].dst_port; }
    return delivered[i].data;
}

/* Q12: the source address and port, which the mutation census showed
 * were reaching the socket layer entirely unchecked. A stack that
 * reports the wrong peer sends every reply to the wrong machine. */
uint16_t fake_socket_delivered_src_port(int i) {
    return (i >= 0 && i < delivered_count) ? delivered[i].src_port : 0;
}

uint32_t fake_socket_delivered_src_ip(int i) {
    return (i >= 0 && i < delivered_count) ? delivered[i].src_ip : 0;
}

void socket_deliver(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                    const uint8_t *data, uint16_t len) {
    if (delivered_count >= MAX_DELIVERED) {
        panic("fake_socket: far more deliveries than any test expects");
    }
    if (len > MAX_PAYLOAD) {
        panic("fake_socket: a delivery larger than the fake can hold");
    }
    delivery_t *d = &delivered[delivered_count++];
    d->src_ip = src_ip;
    d->src_port = src_port;
    d->dst_port = dst_port;
    d->len = len;
    if (len) {
        memcpy(d->data, data, len);
    }
}
