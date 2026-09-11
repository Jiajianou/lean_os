#include "arp.h"

#include "ip.h" /* M116: IP_HEADER_LEN, for the held packet */
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

/* ---- M116: a packet waiting for its neighbour's answer ---------------
 *
 * One per unresolved neighbour, newest wins - BSD's la_hold. Until M116
 * ip.c dropped the packet instead ("send the ARP request, drop this one,
 * and let the caller send again"), which made the first contact with any
 * machine on the local link wait for a retransmission timer: 980 ms to
 * connect one hop away, measured by the [m116] self-test, all of it a
 * SYN that never left. Four slots, because this network has a gateway,
 * a DNS server and at most a couple of peers being resolved at once; a
 * fifth evicts slot 0, and the packet in it is lost exactly as it would
 * have been before.
 *
 * And a neighbour that never answers is reported, not held for ever.
 * Every send to it asks again (resolve_neighbor sends the request), and
 * the third unanswered one is refused - Linux's three probes, counted in
 * questions rather than seconds so that a host test can hold it to the
 * number. The slot is cleared when it gives up, so a neighbour that
 * comes back later is asked afresh. */
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
            slot = i; /* the newer packet replaces the older one */
        }
    }
    if (slot >= 0) {
        if (++arp_hold_slots[slot].asks >= ARP_HOLD_GIVE_UP) {
            arp_hold_slots[slot].used = 0;
            return -1; /* asked three times, never answered: unreachable */
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

/* Sends whatever was waiting for `ip`, now that it has an address. */
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
    /* Cache full: overwrite slot 0 rather than grow it - this network has
     * one gateway and, at most, a couple of self-test peers, nowhere near
     * ARP_CACHE_SIZE entries in practice. */
    arp_cache[0].ip = ip;
    k_memcpy(arp_cache[0].mac, mac, ETH_ADDR_LEN);
    arp_cache[0].valid = 1;
}

/* M116: learning an address - from a reply, a request, or any IP packet
 * the neighbour sends - is also what lets its held packet go. */
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
