#pragma once

#include <stdint.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

#define IP_HEADER_LEN 20

int ip_send(uint32_t dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_len);

int ip_send_from(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                 const uint8_t *payload, uint16_t payload_len);

void ip_handle_packet(const uint8_t *src_mac, const uint8_t *payload, uint16_t len);
