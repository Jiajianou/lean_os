#pragma once

#include <stdint.h>

#include "ethernet.h"

void arp_init(void);

void arp_send_request(uint32_t target_ip);

void arp_handle_packet(const uint8_t *payload, uint16_t length);

void arp_learn(uint32_t ip, const uint8_t mac[ETH_ADDRESS_LENGTH]);

int arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ADDRESS_LENGTH]);

int arp_hold(uint32_t next_hop_ip, const uint8_t *ip_packet, uint16_t length);
