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

/* Standard Internet checksum (RFC 1071): one's-complement sum of every
 * 16-bit word, folding carries back in, then one's complemented. Called
 * with the checksum field itself zeroed. */
static void build_header(uint8_t *packet, uint32_t src_ip, uint32_t dst_ip, uint8_t protocol, uint16_t payload_len);

/* M66: the loopback queue - see the comment in ip_send_from. Depth 32 is
 * comfortably more than one send can set off: a TCP sender is bounded by
 * its congestion window and a receiver answers each segment with at most
 * one ACK, so the queue holds a burst rather than a transfer. */
#define LOOP_QUEUE_DEPTH 32
static uint8_t loop_queue[LOOP_QUEUE_DEPTH][IP_HEADER_LEN + 1500];
static uint16_t loop_lens[LOOP_QUEUE_DEPTH];
static int loop_head, loop_tail, loop_count, loop_draining;

/* Is IF set? The one question that decides whether this code is allowed
 * to wait for anything. Q4: the primitive moved to arch/x86_64/io.h,
 * where irq_save_disable and the rest of them already were. */
#define interrupts_enabled() cpu_interrupts_enabled()

/* Resolves dst_ip's neighbour MAC.
 *
 * M27 wrote this as a bounded `hlt` loop, and that was correct for every
 * caller it had: kernel_main's ICMP self-test, running in kernel context
 * with interrupts on, where halting until the next timer tick is exactly
 * how you wait.
 *
 * M64 gave it a caller in ring 3, and the loop became a way for a user
 * program to hang the machine outright. `int 0x80` goes through an
 * *interrupt* gate (kernel/arch/x86_64/idt.c), so IF is clear for the
 * whole syscall - a `hlt` there halts the CPU with nothing able to wake
 * it, and even a `schedule()` instead would spin forever, because the
 * timer that has to advance the deadline is the interrupt that cannot
 * fire. The first `sendto` to an unresolved address stopped the machine
 * dead, and the boot log ended mid-self-test with no panic and no clue.
 *
 * So: wait only when waiting is possible, and otherwise send the ARP
 * request and let the packet wait for the answer. This paragraph used to
 * say "drop this one, and let the caller send again", and call that what
 * BSD does. It is not - BSD holds the packet (la_hold) - and dropping it
 * cost every first contact with a machine on the local link a whole TCP
 * retransmission timeout, 980 ms to connect one hop away. M116 holds it:
 * see arp_hold, and ip_send_from below.
 *
 * The alternative fix is to make vector 0x80 a trap gate so IF stays set
 * through a syscall, which is what every production kernel does and
 * which this one may well want eventually. It is not a change to make
 * inside a networking milestone: it makes every syscall in the system
 * preemptible at once. */
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

    /* M64: loopback. Anything addressed to us - our own address or
     * 127-anything - goes back up the stack instead of onto the wire,
     * which is both what a real stack does and what makes everything
     * above testable on a machine with no network at all. The MAC handed
     * to ip_handle_packet is our own, which is the truth: we are the
     * sender.
     *
     * M66: and it is a *queue*, which M64 did not need and TCP does.
     *
     * M64's version called ip_handle_packet directly, which is fine for
     * a protocol where delivering a datagram cannot produce another one.
     * TCP is not that protocol: a data segment delivered here makes the
     * receiver send an ACK, which is delivered here, which lets the
     * sender send more data, which is delivered here. A 16 KiB transfer
     * would have run its entire length inside one recursive call chain -
     * dozens of frames deep on a kernel stack that has no room for it,
     * with every level overwriting the one static buffer the level below
     * was still reading out of.
     *
     * So: enqueue, and let the outermost caller drain. Recursion becomes
     * iteration, the buffer belongs to one queue slot rather than to
     * whoever is deepest, and the stack depth is two regardless of how
     * much traffic a single send sets off. A full queue drops the packet,
     * which is what a congested interface does and what every protocol
     * above here already has to handle. */
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
            return 0; /* the frame below us on the stack will get to it */
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

    /* A broadcast has no next hop to resolve - it goes to every station
     * on the segment by definition, which is the only way DHCP can ask
     * for an address before it has one. */
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
        /* M116: held for the answer, not dropped - see arp_hold, and the
         * comment on resolve_neighbor above for what dropping it cost. A
         * neighbour that never answers is reported as -1 by the third
         * send, which is what nettest's "unreachable" check holds it to. */
        return arp_hold(next_hop_ip, packet, (uint16_t)(IP_HEADER_LEN + payload_len));
    }

    eth_send(next_hop_mac, ETH_TYPE_IPV4, packet, (uint16_t)(IP_HEADER_LEN + payload_len));
    return 0;
}

/* Fills in the 20-byte header for a packet of `payload_len` bytes. Split
 * out so the loopback shortcut above builds a *real* header rather than
 * a shorter path around one - a datagram that only works when it does
 * not touch the wire is a datagram whose first real send is untested. */
static void build_header(uint8_t *packet, uint32_t src_ip, uint32_t dst_ip, uint8_t protocol, uint16_t payload_len) {
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

    /* An IP header can claim a total length shorter than the frame it
     * arrived in (Ethernet pads short frames to 60 bytes, so almost every
     * small datagram has trailing padding). Trust the header, not the
     * frame: handing the padding up as payload is how a 4-byte reply
     * becomes a 22-byte one. */
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
