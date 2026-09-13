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

static const uint8_t src_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) {
        return 0;
    }
    uint8_t which = data[0];
    const uint8_t *frame = data + 1;
    size_t len = size - 1;

    if (len > 65535) {
        len = 65535;
    }

    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) {
        return 0;
    }
    memcpy(buf, frame, len);

    fake_net_reset();
    fake_socket_reset();

    switch (which % 6) {
        case 0: eth_receive(buf, (uint16_t)len); break;
        case 1: arp_handle_packet(buf, (uint16_t)len); break;
        case 2: ip_handle_packet(src_mac, buf, (uint16_t)len); break;
        case 3: icmp_handle_packet(0x0A000202u, buf, (uint16_t)len); break;
        case 4: udp_handle_packet(0x0A000202u, 0x0A00020Fu, buf, (uint16_t)len); break;
        case 5: tcp_handle_packet(0x0A000202u, 0x0A00020Fu, buf, (uint16_t)len); break;
        default: break;
    }

    free(buf);
    return 0;
}
