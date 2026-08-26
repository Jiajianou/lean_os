/* kernel/net/ip.h
 *
 * Minimal IPv4: no options on send, options-skipping (not parsing) on
 * receive, no fragmentation either direction - the self-test's ICMP
 * echo request/reply never needs either, and nothing else in this stack
 * sends IP traffic. "Routing" is one on-link/off-link check: ARP the
 * destination directly if it's on our /24, otherwise ARP the gateway and
 * send the frame there instead - real IP routing minus everything this
 * single-subnet-plus-one-gateway network has no use for (a routing
 * table, multiple interfaces).
 *
 * Header fields are read/written byte-by-byte rather than through a
 * packed struct: this kernel only ever runs on little-endian x86_64, so
 * a struct overlay of a big-endian wire format would need its own
 * byteswap on every field access anyway - reading bytes out by hand costs
 * the same and keeps the wire byte order visible at each call site
 * instead of hidden behind a struct definition (arp.c does the same).
 */
#pragma once

#include <stdint.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

#define IP_HEADER_LEN 20 /* no options - see header comment */

/* Builds an IPv4 header (no options) around payload and hands the whole
 * thing to eth_send once the next-hop MAC is resolved (ARPing it first,
 * blocking - bounded poll on arp_lookup - if it isn't already cached).
 * Not safe to call from interrupt context unless the next hop is already
 * cached: nothing in this stack ever calls it uncached from an IRQ
 * handler (icmp.c's echo-reply path relies on arp_handle_packet/
 * arp_learn having already cached the sender from the request that
 * triggered the reply - see arp.h's own comment on why that's guaranteed).
 *
 * M64: returns 0, or -1 for a payload too large or a next hop that would
 * not answer ARP. Both of those used to be panics, from a milestone where
 * the only caller was a boot self-test pinging a gateway that was
 * guaranteed to be there. The moment SYS_sendto exists, "the destination
 * is unreachable" is an ordinary thing a program does - typing an address
 * wrong should not be a way to halt the machine, which is M52's rule
 * arriving in the one subsystem M52 could not reach. */
int ip_send(uint32_t dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_len);

/* M64: the same send with the source address stated rather than taken
 * from the interface. Exactly one caller, and it is the one that has to
 * exist: a DHCP client must put 0.0.0.0 in the header until it has a
 * lease, because RFC 2131 says so and because the address it would
 * otherwise use is a guess it is in the middle of trying to replace.
 * Everything else calls ip_send and should. */
int ip_send_from(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                 const uint8_t *payload, uint16_t payload_len);

/* Called by ethernet.c for a received ETH_TYPE_IPV4 frame. src_mac is the
 * frame's Ethernet source address (for opportunistic ARP learning - see
 * arp.h); payload/len are everything after the Ethernet header, i.e. the
 * IP header itself plus whatever it carries. Drops anything not addressed
 * to our own IP, and anything whose protocol nothing above understands. */
void ip_handle_packet(const uint8_t *src_mac, const uint8_t *payload, uint16_t len);
