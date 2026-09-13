#pragma once

#include <stdint.h>

#define ETH_ADDR_LEN    6
#define ETH_HEADER_LEN  14
#define ETH_MIN_FRAME   60

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806

extern const uint8_t eth_broadcast_mac[ETH_ADDR_LEN];

void eth_send(const uint8_t dst_mac[ETH_ADDR_LEN], uint16_t ethertype, const uint8_t *payload, uint16_t payload_len);

void eth_receive(const uint8_t *frame, uint16_t len);
