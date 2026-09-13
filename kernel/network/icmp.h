#pragma once

#include <stdint.h>

void icmp_send_echo_request(uint32_t destination_ip, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t payload_length);

int icmp_echo_reply_seen(uint16_t id, uint16_t seq);

void icmp_handle_packet(uint32_t source_ip, const uint8_t *payload, uint16_t length);
