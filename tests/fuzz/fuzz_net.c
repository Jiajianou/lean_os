#include "network/arp.h"
#include "network/ethernet.h"
#include "network/icmp.h"
#include "network/ip.h"
#include "network/tcp.h"
#include "network/udp.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void fake_net_reset(void);
void fake_socket_reset(void);

static const uint8_t source_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) {
        return 0;
    }
    uint8_t which = data[0];
    const uint8_t *frame = data + 1;
    size_t length = size - 1;

    if (length > 65535) {
        length = 65535;
    }

    uint8_t *buffer = (uint8_t *)malloc(length ? length : 1);
    if (!buffer) {
        return 0;
    }
    memcpy(buffer, frame, length);

    fake_net_reset();
    fake_socket_reset();

    switch (which % 6) {
        case 0: eth_receive(buffer, (uint16_t)length); break;
        case 1: arp_handle_packet(buffer, (uint16_t)length); break;
        case 2: ip_handle_packet(source_mac, buffer, (uint16_t)length); break;
        case 3: icmp_handle_packet(0x0A000202u, buffer, (uint16_t)length); break;
        case 4: udp_handle_packet(0x0A000202u, 0x0A00020Fu, buffer, (uint16_t)length); break;
        case 5: tcp_handle_packet(0x0A000202u, 0x0A00020Fu, buffer, (uint16_t)length); break;
        default: break;
    }

    free(buffer);
    return 0;
}
