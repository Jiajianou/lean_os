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
