#include "ethernet.h"

#include "arp.h"
#include "ip.h"
#include "library/kernel_library.h"
#include "network.h"

const uint8_t eth_broadcast_mac[ETH_ADDRESS_LENGTH] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void eth_send(const uint8_t destination_mac[ETH_ADDRESS_LENGTH], uint16_t ethertype, const uint8_t *payload, uint16_t payload_length) {
    uint8_t frame[ETH_MAX_FRAME];
    if (payload_length > ETH_MAX_FRAME - ETH_HEADER_LENGTH) {
        return;
    }
    k_memset(frame, 0, sizeof(frame));

    k_memcpy(frame, destination_mac, ETH_ADDRESS_LENGTH);
    k_memcpy(frame + ETH_ADDRESS_LENGTH, net_local_mac(), ETH_ADDRESS_LENGTH);
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)(ethertype & 0xFF);
    k_memcpy(frame + ETH_HEADER_LENGTH, payload, payload_length);

    uint16_t total = (uint16_t)(ETH_HEADER_LENGTH + payload_length);
    if (total < ETH_MIN_FRAME) {
        total = ETH_MIN_FRAME;
    }
    net_link_send(frame, total);
}

void eth_receive(const uint8_t *frame, uint16_t length) {
    if (length < ETH_HEADER_LENGTH) {
        return;
    }
    net_lock_acquire();
    uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
    const uint8_t *payload = frame + ETH_HEADER_LENGTH;
    uint16_t payload_length = (uint16_t)(length - ETH_HEADER_LENGTH);

    if (ethertype == ETH_TYPE_ARP) {
        arp_handle_packet(payload, payload_length);
    } else if (ethertype == ETH_TYPE_IPV4) {
        ip_handle_packet(frame + ETH_ADDRESS_LENGTH, payload, payload_length);
    }
    net_lock_release();
}
