#include "ip.h"

#include "arp.h"
#include "drivers/pit.h"
#include "ethernet.h"
#include "icmp.h"
#include "lib/libk.h"
#include "net.h"
#include "panic.h"

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Standard Internet checksum (RFC 1071): one's-complement sum of every
 * 16-bit word, folding carries back in, then one's complemented. Called
 * with the checksum field itself zeroed. */
static uint16_t checksum16(const uint8_t *data, uint16_t len) {
    uint32_t sum = 0;
    for (uint16_t i = 0; i + 1 < len; i += 2) {
        sum += (uint16_t)((data[i] << 8) | data[i + 1]);
    }
    if (len & 1) {
        sum += (uint16_t)(data[len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Resolves dst_ip's neighbor MAC, ARPing and blocking (bounded) for a
 * reply if it isn't already cached - see ip.h's comment on why this is
 * only safe from normal (non-interrupt) context. */
static int resolve_neighbor(uint32_t next_hop_ip, uint8_t mac_out[ETH_ADDR_LEN]) {
    if (arp_lookup(next_hop_ip, mac_out)) {
        return 1;
    }
    arp_send_request(next_hop_ip);
    uint64_t deadline = pit_get_ticks() + 2 * PIT_HZ;
    while (pit_get_ticks() < deadline) {
        if (arp_lookup(next_hop_ip, mac_out)) {
            return 1;
        }
        __asm__ volatile("hlt");
    }
    return 0;
}

void ip_send(uint32_t dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_len) {
    uint32_t next_hop_ip = ((dst_ip ^ net_local_ip()) & NET_SUBNET_MASK) == 0 ? dst_ip : NET_GATEWAY_IP;

    uint8_t next_hop_mac[ETH_ADDR_LEN];
    if (!resolve_neighbor(next_hop_ip, next_hop_mac)) {
        panic("ip_send: ARP resolution timed out");
    }

    static uint8_t packet[IP_HEADER_LEN + 1500];
    if ((uint32_t)(IP_HEADER_LEN + payload_len) > sizeof(packet)) {
        panic("ip_send: payload too large");
    }

    static uint16_t identification;
    identification++;

    packet[0] = 0x45; /* version 4, IHL 5 (20-byte header, no options) */
    packet[1] = 0;    /* DSCP/ECN */
    uint16_t total_len = (uint16_t)(IP_HEADER_LEN + payload_len);
    packet[2] = (uint8_t)(total_len >> 8);
    packet[3] = (uint8_t)(total_len & 0xFF);
    packet[4] = (uint8_t)(identification >> 8);
    packet[5] = (uint8_t)(identification & 0xFF);
    packet[6] = 0; /* flags/fragment offset: no fragmentation */
    packet[7] = 0;
    packet[8] = 64; /* TTL */
    packet[9] = protocol;
    packet[10] = 0; /* checksum, filled in below */
    packet[11] = 0;
    write_be32(packet + 12, net_local_ip());
    write_be32(packet + 16, dst_ip);

    uint16_t csum = checksum16(packet, IP_HEADER_LEN);
    packet[10] = (uint8_t)(csum >> 8);
    packet[11] = (uint8_t)(csum & 0xFF);

    k_memcpy(packet + IP_HEADER_LEN, payload, payload_len);

    eth_send(next_hop_mac, ETH_TYPE_IPV4, packet, total_len);
}

void ip_handle_packet(const uint8_t *src_mac, const uint8_t *payload, uint16_t len) {
    if (len < IP_HEADER_LEN) {
        return;
    }
    uint8_t version = (uint8_t)(payload[0] >> 4);
    uint8_t ihl = (uint8_t)(payload[0] & 0x0F);
    if (version != 4 || ihl < 5) {
        return;
    }
    uint16_t header_len = (uint16_t)(ihl * 4);
    if (len < header_len) {
        return;
    }

    uint8_t protocol = payload[9];
    uint32_t src_ip = read_be32(payload + 12);
    uint32_t dst_ip = read_be32(payload + 16);

    if (dst_ip != net_local_ip()) {
        return;
    }

    arp_learn(src_ip, src_mac);

    if (protocol == IP_PROTO_ICMP) {
        icmp_handle_packet(src_ip, payload + header_len, (uint16_t)(len - header_len));
    }
}
