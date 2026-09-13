#include "arp.h"

#include "ip.h"
#include "library/kernel_library.h"
#include "network.h"
#include "wire.h"

#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IPV4     0x0800
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2

#define ARP_PACKET_LEN 28

#define ARP_CACHE_SIZE 8
static struct {
    uint32_t ip;
    uint8_t mac[ETH_ADDR_LEN];
    int valid;
} arp_cache[ARP_CACHE_SIZE];

#define ARP_HOLD_SLOTS   4
#define ARP_HOLD_MAX     (IP_HEADER_LEN + 1500)
#define ARP_HOLD_GIVE_UP 3
static struct {
    uint32_t ip;
    uint16_t len;
    int used;
    int asks;
    uint8_t packet[ARP_HOLD_MAX];
} arp_hold_slots[ARP_HOLD_SLOTS];

void arp_init(void) {
    k_memset(arp_cache, 0, sizeof(arp_cache));
    k_memset(arp_hold_slots, 0, sizeof(arp_hold_slots));
}

int arp_hold(uint32_t next_hop_ip, const uint8_t *ip_packet, uint16_t len) {
    if (len > ARP_HOLD_MAX) {
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < ARP_HOLD_SLOTS && slot < 0; i++) {
        if (arp_hold_slots[i].used && arp_hold_slots[i].ip == next_hop_ip) {
            slot = i;
        }
    }
    if (slot >= 0) {
        if (++arp_hold_slots[slot].asks >= ARP_HOLD_GIVE_UP) {
            arp_hold_slots[slot].used = 0;
            return -1;
        }
    } else {
        for (int i = 0; i < ARP_HOLD_SLOTS && slot < 0; i++) {
            if (!arp_hold_slots[i].used) {
                slot = i;
            }
        }
        if (slot < 0) {
            slot = 0;
        }
        arp_hold_slots[slot].asks = 1;
    }
    arp_hold_slots[slot].ip = next_hop_ip;
    arp_hold_slots[slot].len = len;
    arp_hold_slots[slot].used = 1;
    k_memcpy(arp_hold_slots[slot].packet, ip_packet, len);
    return 0;
}

static void release_held(uint32_t ip, const uint8_t mac[ETH_ADDR_LEN]) {
    for (int i = 0; i < ARP_HOLD_SLOTS; i++) {
        if (arp_hold_slots[i].used && arp_hold_slots[i].ip == ip) {
            arp_hold_slots[i].used = 0;
            eth_send(mac, ETH_TYPE_IPV4, arp_hold_slots[i].packet, arp_hold_slots[i].len);
        }
    }
}

static void cache_store(uint32_t ip, const uint8_t mac[ETH_ADDR_LEN]) {
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
    arp_cache[0].ip = ip;
    k_memcpy(arp_cache[0].mac, mac, ETH_ADDR_LEN);
    arp_cache[0].valid = 1;
}

void arp_learn(uint32_t ip, const uint8_t mac[ETH_ADDR_LEN]) {
    cache_store(ip, mac);
    release_held(ip, mac);
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
    packet[4] = ETH_ADDR_LEN;
    packet[5] = 4;
    packet[6] = (uint8_t)(op >> 8);
    packet[7] = (uint8_t)(op & 0xFF);
    k_memcpy(packet + 8, net_local_mac(), ETH_ADDR_LEN);
    net_write_be32(packet + 14, net_local_ip());
    k_memcpy(packet + 18, dst_mac, ETH_ADDR_LEN);
    net_write_be32(packet + 24, target_ip);

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
