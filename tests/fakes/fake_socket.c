#include "network/socket.h"

#include <stdint.h>
#include <string.h>

void panic(const char *message);

#define MAX_DELIVERED 32
#define MAX_PAYLOAD 2048

typedef struct {
    uint32_t source_ip;
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t length;
    uint8_t data[MAX_PAYLOAD];
} delivery_t;

static delivery_t delivered[MAX_DELIVERED];
static int delivered_count;

void fake_socket_reset(void);
int fake_socket_delivered_count(void);
const uint8_t *fake_socket_delivered(int i, uint32_t *length_out, uint16_t *destination_port_out);
uint16_t fake_socket_delivered_source_port(int i);
uint32_t fake_socket_delivered_source_ip(int i);

void fake_socket_reset(void) { delivered_count = 0; }
int fake_socket_delivered_count(void) { return delivered_count; }

const uint8_t *fake_socket_delivered(int i, uint32_t *length_out, uint16_t *destination_port_out) {
    if (i < 0 || i >= delivered_count) {
        return NULL;
    }
    if (length_out) { *length_out = delivered[i].length; }
    if (destination_port_out) { *destination_port_out = delivered[i].destination_port; }
    return delivered[i].data;
}

uint16_t fake_socket_delivered_source_port(int i) {
    return (i >= 0 && i < delivered_count) ? delivered[i].source_port : 0;
}

uint32_t fake_socket_delivered_source_ip(int i) {
    return (i >= 0 && i < delivered_count) ? delivered[i].source_ip : 0;
}

void socket_deliver(uint16_t destination_port, uint32_t source_ip, uint16_t source_port,
                    const uint8_t *data, uint16_t length) {
    if (delivered_count >= MAX_DELIVERED) {
        panic("fake_socket: far more deliveries than any test expects");
    }
    if (length > MAX_PAYLOAD) {
        panic("fake_socket: a delivery larger than the fake can hold");
    }
    delivery_t *d = &delivered[delivered_count++];
    d->source_ip = source_ip;
    d->source_port = source_port;
    d->destination_port = destination_port;
    d->length = length;
    if (length) {
        memcpy(d->data, data, length);
    }
}
