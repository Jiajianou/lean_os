#include "icmp.h"

#include "ip.h"
#include "library/kernel_library.h"
#include "wire.h"

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

#define ICMP_HEADER_LENGTH 8
#define ICMP_MAX_PAYLOAD 1024

static volatile uint16_t last_reply_id;
static volatile uint16_t last_reply_sequence;
static volatile int last_reply_valid;

static void send_icmp(uint32_t destination_ip, uint8_t type, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t payload_length) {
    static uint8_t packet[ICMP_HEADER_LENGTH + ICMP_MAX_PAYLOAD];
    if (payload_length > ICMP_MAX_PAYLOAD) {
        payload_length = ICMP_MAX_PAYLOAD;
    }

    packet[0] = type;
    packet[1] = 0;
    packet[2] = 0;
    packet[3] = 0;
    packet[4] = (uint8_t)(id >> 8);
    packet[5] = (uint8_t)(id & 0xFF);
    packet[6] = (uint8_t)(seq >> 8);
    packet[7] = (uint8_t)(seq & 0xFF);
    if (payload_length > 0) {
        k_memcpy(packet + ICMP_HEADER_LENGTH, payload, payload_length);
    }

    uint16_t total_length = (uint16_t)(ICMP_HEADER_LENGTH + payload_length);
    uint16_t csum = net_checksum16(packet, total_length);
    packet[2] = (uint8_t)(csum >> 8);
    packet[3] = (uint8_t)(csum & 0xFF);

    ip_send(destination_ip, IP_PROTO_ICMP, packet, total_length);
}

void icmp_send_echo_request(uint32_t destination_ip, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t payload_length) {
    send_icmp(destination_ip, ICMP_TYPE_ECHO_REQUEST, id, seq, payload, payload_length);
}

int icmp_echo_reply_seen(uint16_t id, uint16_t seq) {
    if (last_reply_valid && last_reply_id == id && last_reply_sequence == seq) {
        last_reply_valid = 0;
        return 1;
    }
    return 0;
}

void icmp_handle_packet(uint32_t source_ip, const uint8_t *payload, uint16_t length) {
    if (length < ICMP_HEADER_LENGTH) {
        return;
    }
    uint8_t type = payload[0];
    uint16_t id = (uint16_t)((payload[4] << 8) | payload[5]);
    uint16_t seq = (uint16_t)((payload[6] << 8) | payload[7]);

    if (type == ICMP_TYPE_ECHO_REQUEST) {
        send_icmp(source_ip, ICMP_TYPE_ECHO_REPLY, id, seq, payload + ICMP_HEADER_LENGTH, (uint16_t)(length - ICMP_HEADER_LENGTH));
    } else if (type == ICMP_TYPE_ECHO_REPLY) {
        last_reply_id = id;
        last_reply_sequence = seq;
        last_reply_valid = 1;
    }
}
