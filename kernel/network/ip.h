#pragma once

#include <stdint.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

#define IP_HEADER_LEN 20

int ip_send(uint32_t destination_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_length);

int ip_send_from(uint32_t source_ip, uint32_t destination_ip, uint8_t protocol,
                 const uint8_t *payload, uint16_t payload_length);

void ip_handle_packet(const uint8_t *source_mac, const uint8_t *payload, uint16_t len);
