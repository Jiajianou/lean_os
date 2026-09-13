#include "icmp.h"

#include "ip.h"
#include "library/kernel_library.h"
#include "wire.h"

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

#define ICMP_HEADER_LEN 8
#define ICMP_MAX_PAYLOAD 1024

static volatile uint16_t last_reply_id;
static volatile uint16_t last_reply_seq;
static volatile int last_reply_valid;

static void send_icmp(uint32_t dst_ip, uint8_t type, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t payload_len) {
    static uint8_t packet[ICMP_HEADER_LEN + ICMP_MAX_PAYLOAD];
    if (payload_len > ICMP_MAX_PAYLOAD) {
        payload_len = ICMP_MAX_PAYLOAD;
    }

    packet[0] = type;
    packet[1] = 0;
    packet[2] = 0;
    packet[3] = 0;
    packet[4] = (uint8_t)(id >> 8);
    packet[5] = (uint8_t)(id & 0xFF);
    packet[6] = (uint8_t)(seq >> 8);
    packet[7] = (uint8_t)(seq & 0xFF);
    if (payload_len > 0) {
        k_memcpy(packet + ICMP_HEADER_LEN, payload, payload_len);
    }

    uint16_t total_len = (uint16_t)(ICMP_HEADER_LEN + payload_len);
    uint16_t csum = net_checksum16(packet, total_len);
    packet[2] = (uint8_t)(csum >> 8);
    packet[3] = (uint8_t)(csum & 0xFF);

    ip_send(dst_ip, IP_PROTO_ICMP, packet, total_len);
}

void icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t payload_len) {
    send_icmp(dst_ip, ICMP_TYPE_ECHO_REQUEST, id, seq, payload, payload_len);
}

int icmp_echo_reply_seen(uint16_t id, uint16_t seq) {
    if (last_reply_valid && last_reply_id == id && last_reply_seq == seq) {
        last_reply_valid = 0;
        return 1;
    }
    return 0;
}

void icmp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len) {
    if (len < ICMP_HEADER_LEN) {
        return;
    }
    uint8_t type = payload[0];
    uint16_t id = (uint16_t)((payload[4] << 8) | payload[5]);
    uint16_t seq = (uint16_t)((payload[6] << 8) | payload[7]);

    if (type == ICMP_TYPE_ECHO_REQUEST) {
        send_icmp(src_ip, ICMP_TYPE_ECHO_REPLY, id, seq, payload + ICMP_HEADER_LEN, (uint16_t)(len - ICMP_HEADER_LEN));
    } else if (type == ICMP_TYPE_ECHO_REPLY) {
        last_reply_id = id;
        last_reply_seq = seq;
        last_reply_valid = 1;
    }
}
