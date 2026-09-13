#pragma once

#include <stdint.h>

#define UDP_HEADER_LENGTH 8
#define UDP_MAX_PAYLOAD 1472

int udp_send(uint32_t destination_ip, uint16_t destination_port, uint16_t source_port,
             const uint8_t *payload, uint16_t payload_length);

int udp_send_from(uint32_t source_ip, uint32_t destination_ip, uint16_t destination_port, uint16_t source_port,
                  const uint8_t *payload, uint16_t payload_length);

void udp_handle_packet(uint32_t source_ip, uint32_t destination_ip, const uint8_t *payload, uint16_t length);
