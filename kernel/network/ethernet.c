#include "ethernet.h"

#include "arp.h"
#include "drivers/rtl8139.h"
#include "ip.h"
#include "library/kernel_library.h"
#include "network.h"

const uint8_t eth_broadcast_mac[ETH_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void eth_send(const uint8_t destination_mac[ETH_ADDR_LEN], uint16_t ethertype, const uint8_t *payload, uint16_t payload_length) {
    uint8_t frame[ETH_MIN_FRAME > RTL8139_MAX_FRAME ? ETH_MIN_FRAME : RTL8139_MAX_FRAME];
    k_memset(frame, 0, sizeof(frame));

    k_memcpy(frame, destination_mac, ETH_ADDR_LEN);
    k_memcpy(frame + ETH_ADDR_LEN, net_local_mac(), ETH_ADDR_LEN);
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)(ethertype & 0xFF);
    k_memcpy(frame + ETH_HEADER_LEN, payload, payload_length);

    uint16_t total = (uint16_t)(ETH_HEADER_LEN + payload_length);
    if (total < ETH_MIN_FRAME) {
        total = ETH_MIN_FRAME;
    }
    rtl8139_send(frame, total);
}

void eth_receive(const uint8_t *frame, uint16_t len) {
    if (len < ETH_HEADER_LEN) {
        return;
    }
    net_lock_acquire();
    uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
    const uint8_t *payload = frame + ETH_HEADER_LEN;
    uint16_t payload_length = (uint16_t)(len - ETH_HEADER_LEN);

    if (ethertype == ETH_TYPE_ARP) {
        arp_handle_packet(payload, payload_length);
    } else if (ethertype == ETH_TYPE_IPV4) {
        ip_handle_packet(frame + ETH_ADDR_LEN, payload, payload_length);
    }
    net_lock_release();
}
