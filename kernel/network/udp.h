#pragma once

#include <stdint.h>

#define UDP_HEADER_LEN 8
#define UDP_MAX_PAYLOAD 1472

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const uint8_t *payload, uint16_t payload_len);

int udp_send_from(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                  const uint8_t *payload, uint16_t payload_len);

void udp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *payload, uint16_t len);
