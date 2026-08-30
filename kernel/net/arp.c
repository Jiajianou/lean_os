#include "arp.h"

#include "lib/libk.h"
#include "net.h"
#include "wire.h"

#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IPV4     0x0800
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2

/* Wire layout, all multi-byte fields big-endian (network byte order) -
 * packed manually field by field rather than a struct + attribute
 * packed, matching how ip.c/icmp.c read their headers too (see ip.c's
 * comment on why: this kernel only targets little-endian x86_64, so any
 * struct overlay would need its own byteswap helpers anyway - reading
 * bytes out by hand is no more code and makes the wire format's byte
 * order visible at the call site instead of hidden in a struct). */
#define ARP_PACKET_LEN 28

#define ARP_CACHE_SIZE 8
static struct {
    uint32_t ip;
    uint8_t mac[ETH_ADDR_LEN];
    int valid;
} arp_cache[ARP_CACHE_SIZE];

void arp_init(void) {
    k_memset(arp_cache, 0, sizeof(arp_cache));
}

void arp_learn(uint32_t ip, const uint8_t mac[ETH_ADDR_LEN]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            k_memcpy(arp_cache[i].mac, mac, ETH_ADDR_LEN);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            k_memcpy(arp_cache[i].mac, mac, ETH_ADDR_LEN);
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Cache full: overwrite slot 0 rather than grow it - this network has
     * one gateway and, at most, a couple of self-test peers, nowhere near
     * ARP_CACHE_SIZE entries in practice. */
    arp_cache[0].ip = ip;
    k_memcpy(arp_cache[0].mac, mac, ETH_ADDR_LEN);
    arp_cache[0].valid = 1;
}

int arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ADDR_LEN]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            k_memcpy(mac_out, arp_cache[i].mac, ETH_ADDR_LEN);
            return 1;
        }
    }
    return 0;
}

static void build_and_send(uint16_t op, const uint8_t dst_mac[ETH_ADDR_LEN], uint32_t target_ip) {
    uint8_t packet[ARP_PACKET_LEN];
    packet[0] = ARP_HTYPE_ETHERNET >> 8;
    packet[1] = ARP_HTYPE_ETHERNET & 0xFF;
    packet[2] = ARP_PTYPE_IPV4 >> 8;
    packet[3] = ARP_PTYPE_IPV4 & 0xFF;
    packet[4] = ETH_ADDR_LEN; /* hlen */
    packet[5] = 4;            /* plen */
    packet[6] = (uint8_t)(op >> 8);
    packet[7] = (uint8_t)(op & 0xFF);
    k_memcpy(packet + 8, net_local_mac(), ETH_ADDR_LEN);   /* sha */
    net_write_be32(packet + 14, net_local_ip());                /* spa */
    k_memcpy(packet + 18, dst_mac, ETH_ADDR_LEN);            /* tha */
    net_write_be32(packet + 24, target_ip);                      /* tpa */

    eth_send(dst_mac, ETH_TYPE_ARP, packet, ARP_PACKET_LEN);
}

void arp_send_request(uint32_t target_ip) {
    build_and_send(ARP_OP_REQUEST, eth_broadcast_mac, target_ip);
}

void arp_handle_packet(const uint8_t *payload, uint16_t len) {
    if (len < ARP_PACKET_LEN) {
        return;
    }
    uint16_t htype = (uint16_t)((payload[0] << 8) | payload[1]);
    uint16_t ptype = (uint16_t)((payload[2] << 8) | payload[3]);
    if (htype != ARP_HTYPE_ETHERNET || ptype != ARP_PTYPE_IPV4) {
        return;
    }
    uint16_t op = (uint16_t)((payload[6] << 8) | payload[7]);
    const uint8_t *sha = payload + 8;
    uint32_t spa = net_read_be32(payload + 14);
    uint32_t tpa = net_read_be32(payload + 24);

    arp_learn(spa, sha);

    if (op == ARP_OP_REQUEST && tpa == net_local_ip()) {
        build_and_send(ARP_OP_REPLY, sha, spa);
    }
}
