#include "ip.h"

#include "architecture/x86_64/io.h"

#include "arp.h"
#include "drivers/pit.h"
#include "ethernet.h"
#include "icmp.h"
#include "library/kernel_library.h"
#include "network.h"
#include "tcp.h"
#include "udp.h"
#include "wire.h"

static void build_header(uint8_t *packet, uint32_t source_ip, uint32_t destination_ip, uint8_t protocol, uint16_t payload_length);

#define LOOP_QUEUE_DEPTH 32
static uint8_t loop_queue[LOOP_QUEUE_DEPTH][IP_HEADER_LENGTH + 1500];
static uint16_t loop_lens[LOOP_QUEUE_DEPTH];
static int loop_head, loop_tail, loop_count, loop_draining;

#define interrupts_enabled() cpu_interrupts_enabled()

static int resolve_neighbor(uint32_t next_hop_ip, uint8_t mac_out[ETH_ADDRESS_LENGTH]) {
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

int ip_send(uint32_t destination_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_length) {
    return ip_send_from(net_local_ip(), destination_ip, protocol, payload, payload_length);
}

int ip_send_from(uint32_t source_ip, uint32_t destination_ip, uint8_t protocol,
                 const uint8_t *payload, uint16_t payload_length) {
    static uint8_t packet[IP_HEADER_LENGTH + 1500];
    if ((uint32_t)(IP_HEADER_LENGTH + payload_length) > sizeof(packet)) {
        return -1;
    }

    if (net_is_local_ip(destination_ip) && destination_ip != NET_BROADCAST_IP) {
        uint16_t total = (uint16_t)(IP_HEADER_LENGTH + payload_length);
        if (loop_count >= LOOP_QUEUE_DEPTH) {
            return -1;
        }
        uint8_t *slot = loop_queue[loop_head];
        build_header(slot, source_ip, destination_ip, protocol, payload_length);
        k_memcpy(slot + IP_HEADER_LENGTH, payload, payload_length);
        loop_lens[loop_head] = total;
        loop_head = (loop_head + 1) % LOOP_QUEUE_DEPTH;
        loop_count++;

        if (loop_draining) {
            return 0;
        }
        loop_draining = 1;
        while (loop_count > 0) {
            uint8_t *next = loop_queue[loop_tail];
            uint16_t next_length = loop_lens[loop_tail];
            loop_tail = (loop_tail + 1) % LOOP_QUEUE_DEPTH;
            loop_count--;
            ip_handle_packet(net_local_mac(), next, next_length);
        }
        loop_draining = 0;
        return 0;
    }

    uint32_t next_hop_ip = ((destination_ip ^ net_local_ip()) & net_subnet_mask()) == 0 ? destination_ip : net_gateway_ip();

    uint8_t next_hop_mac[ETH_ADDRESS_LENGTH];
    int resolved = 1;
    if (destination_ip == NET_BROADCAST_IP) {
        k_memcpy(next_hop_mac, eth_broadcast_mac, ETH_ADDRESS_LENGTH);
    } else if (!resolve_neighbor(next_hop_ip, next_hop_mac)) {
        resolved = 0;
    }

    build_header(packet, source_ip, destination_ip, protocol, payload_length);
    k_memcpy(packet + IP_HEADER_LENGTH, payload, payload_length);

    if (!resolved) {
        return arp_hold(next_hop_ip, packet, (uint16_t)(IP_HEADER_LENGTH + payload_length));
    }

    eth_send(next_hop_mac, ETH_TYPE_IPV4, packet, (uint16_t)(IP_HEADER_LENGTH + payload_length));
    return 0;
}

static void build_header(uint8_t *packet, uint32_t source_ip, uint32_t destination_ip, uint8_t protocol, uint16_t payload_length) {
    static uint16_t identification;
    identification++;

    packet[0] = 0x45;
    packet[1] = 0;
    uint16_t total_length = (uint16_t)(IP_HEADER_LENGTH + payload_length);
    packet[2] = (uint8_t)(total_length >> 8);
    packet[3] = (uint8_t)(total_length & 0xFF);
    packet[4] = (uint8_t)(identification >> 8);
    packet[5] = (uint8_t)(identification & 0xFF);
    packet[6] = 0;
    packet[7] = 0;
    packet[8] = 64;
    packet[9] = protocol;
    packet[10] = 0;
    packet[11] = 0;
    net_write_be32(packet + 12, source_ip);
    net_write_be32(packet + 16, destination_ip);

    uint16_t csum = net_checksum16(packet, IP_HEADER_LENGTH);
    packet[10] = (uint8_t)(csum >> 8);
    packet[11] = (uint8_t)(csum & 0xFF);
}

void ip_handle_packet(const uint8_t *source_mac, const uint8_t *payload, uint16_t length) {
    if (length < IP_HEADER_LENGTH) {
        return;
    }
    uint8_t version = (uint8_t)(payload[0] >> 4);
    uint8_t ihl = (uint8_t)(payload[0] & 0x0F);
    if (version != 4 || ihl < 5) {
        return;
    }
    uint16_t header_length = (uint16_t)(ihl * 4);
    if (length < header_length) {
        return;
    }

    uint8_t protocol = payload[9];
    uint32_t source_ip = net_read_be32(payload + 12);
    uint32_t destination_ip = net_read_be32(payload + 16);

    if (!net_is_local_ip(destination_ip)) {
        return;
    }

    arp_learn(source_ip, source_mac);

    uint16_t total_length = (uint16_t)((payload[2] << 8) | payload[3]);
    if (total_length >= header_length && total_length <= length) {
        length = total_length;
    }

    if (protocol == IP_PROTO_ICMP) {
        icmp_handle_packet(source_ip, payload + header_length, (uint16_t)(length - header_length));
    } else if (protocol == IP_PROTO_UDP) {
        udp_handle_packet(source_ip, destination_ip, payload + header_length, (uint16_t)(length - header_length));
    } else if (protocol == IP_PROTO_TCP) {
        tcp_handle_packet(source_ip, destination_ip, payload + header_length, (uint16_t)(length - header_length));
    }
}
