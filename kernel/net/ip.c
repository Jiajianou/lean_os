#include "ip.h"

#include "arch/x86_64/io.h"

#include "arp.h"
#include "drivers/pit.h"
#include "ethernet.h"
#include "icmp.h"
#include "lib/libk.h"
#include "net.h"
#include "tcp.h"
#include "udp.h"
#include "wire.h"

static void build_header(uint8_t *packet, uint32_t src_ip, uint32_t dst_ip, uint8_t protocol, uint16_t payload_len);

#define LOOP_QUEUE_DEPTH 32
static uint8_t loop_queue[LOOP_QUEUE_DEPTH][IP_HEADER_LEN + 1500];
static uint16_t loop_lens[LOOP_QUEUE_DEPTH];
static int loop_head, loop_tail, loop_count, loop_draining;

#define interrupts_enabled() cpu_interrupts_enabled()

static int resolve_neighbor(uint32_t next_hop_ip, uint8_t mac_out[ETH_ADDR_LEN]) {
    if (arp_lookup(next_hop_ip, mac_out)) {
        return 1;
    }
    arp_send_request(next_hop_ip);
    if (!interrupts_enabled()) {
        return 0;
    }
    uint64_t deadline = pit_get_ticks() + 2 * PIT_HZ;
    while (pit_get_ticks() < deadline) {
        if (arp_lookup(next_hop_ip, mac_out)) {
            return 1;
        }
        cpu_halt();
    }
    return 0;
}

int ip_send(uint32_t dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_len) {
    return ip_send_from(net_local_ip(), dst_ip, protocol, payload, payload_len);
}

int ip_send_from(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                 const uint8_t *payload, uint16_t payload_len) {
    static uint8_t packet[IP_HEADER_LEN + 1500];
    if ((uint32_t)(IP_HEADER_LEN + payload_len) > sizeof(packet)) {
        return -1;
    }

    if (net_is_local_ip(dst_ip) && dst_ip != NET_BROADCAST_IP) {
        uint16_t total = (uint16_t)(IP_HEADER_LEN + payload_len);
        if (loop_count >= LOOP_QUEUE_DEPTH) {
            return -1;
        }
        uint8_t *slot = loop_queue[loop_head];
        build_header(slot, src_ip, dst_ip, protocol, payload_len);
        k_memcpy(slot + IP_HEADER_LEN, payload, payload_len);
        loop_lens[loop_head] = total;
        loop_head = (loop_head + 1) % LOOP_QUEUE_DEPTH;
        loop_count++;

        if (loop_draining) {
            return 0;
        }
        loop_draining = 1;
        while (loop_count > 0) {
            uint8_t *next = loop_queue[loop_tail];
            uint16_t next_len = loop_lens[loop_tail];
            loop_tail = (loop_tail + 1) % LOOP_QUEUE_DEPTH;
            loop_count--;
            ip_handle_packet(net_local_mac(), next, next_len);
        }
        loop_draining = 0;
        return 0;
    }

    uint32_t next_hop_ip = ((dst_ip ^ net_local_ip()) & net_subnet_mask()) == 0 ? dst_ip : net_gateway_ip();

    uint8_t next_hop_mac[ETH_ADDR_LEN];
    int resolved = 1;
    if (dst_ip == NET_BROADCAST_IP) {
        k_memcpy(next_hop_mac, eth_broadcast_mac, ETH_ADDR_LEN);
    } else if (!resolve_neighbor(next_hop_ip, next_hop_mac)) {
        resolved = 0;
    }

    build_header(packet, src_ip, dst_ip, protocol, payload_len);
    k_memcpy(packet + IP_HEADER_LEN, payload, payload_len);

    if (!resolved) {
        return arp_hold(next_hop_ip, packet, (uint16_t)(IP_HEADER_LEN + payload_len));
    }

    eth_send(next_hop_mac, ETH_TYPE_IPV4, packet, (uint16_t)(IP_HEADER_LEN + payload_len));
    return 0;
}

static void build_header(uint8_t *packet, uint32_t src_ip, uint32_t dst_ip, uint8_t protocol, uint16_t payload_len) {
    static uint16_t identification;
    identification++;

    packet[0] = 0x45;
    packet[1] = 0;
    uint16_t total_len = (uint16_t)(IP_HEADER_LEN + payload_len);
    packet[2] = (uint8_t)(total_len >> 8);
    packet[3] = (uint8_t)(total_len & 0xFF);
    packet[4] = (uint8_t)(identification >> 8);
    packet[5] = (uint8_t)(identification & 0xFF);
    packet[6] = 0;
    packet[7] = 0;
    packet[8] = 64;
    packet[9] = protocol;
    packet[10] = 0;
    packet[11] = 0;
    net_write_be32(packet + 12, src_ip);
    net_write_be32(packet + 16, dst_ip);

    uint16_t csum = net_checksum16(packet, IP_HEADER_LEN);
    packet[10] = (uint8_t)(csum >> 8);
    packet[11] = (uint8_t)(csum & 0xFF);
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
    uint32_t src_ip = net_read_be32(payload + 12);
    uint32_t dst_ip = net_read_be32(payload + 16);

    if (!net_is_local_ip(dst_ip)) {
        return;
    }

    arp_learn(src_ip, src_mac);

    uint16_t total_len = (uint16_t)((payload[2] << 8) | payload[3]);
    if (total_len >= header_len && total_len <= len) {
        len = total_len;
    }

    if (protocol == IP_PROTO_ICMP) {
        icmp_handle_packet(src_ip, payload + header_len, (uint16_t)(len - header_len));
    } else if (protocol == IP_PROTO_UDP) {
        udp_handle_packet(src_ip, dst_ip, payload + header_len, (uint16_t)(len - header_len));
    } else if (protocol == IP_PROTO_TCP) {
        tcp_handle_packet(src_ip, dst_ip, payload + header_len, (uint16_t)(len - header_len));
    }
}
