/* kernel/net/arp.h
 *
 * RFC 826 Address Resolution Protocol: IPv4-to-MAC resolution over
 * Ethernet only (htype=1, ptype=0x0800) - the one combination anything in
 * this kernel ever needs. A tiny fixed-size cache (this network has one
 * gateway and, in the self-test, nothing else to talk to) rather than a
 * dynamically sized table with LRU eviction.
 */
#pragma once

#include <stdint.h>

#include "ethernet.h"

void arp_init(void);

/* Broadcasts a "who has target_ip" request. Doesn't wait for the reply -
 * ip_send is what polls arp_lookup afterward (see its own comment on why
 * that split, rather than a blocking resolve-and-send here, is what lets
 * a reply arriving asynchronously in the RX IRQ handler populate the same
 * cache a normal-context caller is spinning on). */
void arp_send_request(uint32_t target_ip);

/* Handles a received ARP packet: answers a request for our own IP, and
 * opportunistically caches the sender's IP/MAC from *any* ARP packet
 * (request or reply) - standard ARP behavior, and what lets an incoming
 * ping's echo reply (built and sent from IRQ context, where blocking on a
 * fresh ARP round trip isn't an option) find the sender's MAC already
 * cached from the ARP request or IP packet that got us the ping at all. */
void arp_handle_packet(const uint8_t *payload, uint16_t len);

/* Opportunistic cache update from a received IPv4 packet's source
 * IP/MAC, for the same reason as above - a host can send IP traffic
 * without ever ARPing us first if it already knows our MAC from an
 * earlier exchange. */
void arp_learn(uint32_t ip, const uint8_t mac[ETH_ADDR_LEN]);

/* Returns 1 and fills mac_out if ip is cached, 0 otherwise. Never blocks -
 * callers that need a resolution to eventually appear (ip_send) poll this
 * in a loop themselves. */
int arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ADDR_LEN]);

/* M116: keeps a whole IPv4 packet for `next_hop_ip` until its MAC is
 * learned, then sends it - one per neighbour, the newest replacing the
 * older, BSD's la_hold. What ip_send does with a packet whose next hop
 * has not answered yet, instead of dropping it. Returns 0 if held, -1 if
 * the neighbour has now been asked three times without answering - it is
 * unreachable, and the caller is told so. */
int arp_hold(uint32_t next_hop_ip, const uint8_t *ip_packet, uint16_t len);
