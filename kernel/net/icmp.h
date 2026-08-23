/* kernel/net/icmp.h
 *
 * ICMP echo request/reply (RFC 792) - "ping", both directions: this
 * kernel can originate a ping (used by the boot self-test to prove the
 * whole stack end to end against QEMU's usermode-networking gateway) and
 * answer one from anywhere else on the network (so a real ping from the
 * host, if the QEMU netdev backend in use forwards it, gets a reply).
 * Nothing else in ICMP (destination-unreachable, TTL-exceeded, ...) is
 * implemented - out of scope for the "driver + ARP + ICMP (ping)" stretch
 * goal.
 */
#pragma once

#include <stdint.h>

/* Sends an echo request to dst_ip carrying (id, seq) and payload -
 * icmp_handle_packet matches a later reply against the same pair. Blocks
 * the same way ip_send does (ARP resolution) - see ip.h's comment on
 * interrupt-context safety. */
void icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t payload_len);

/* Returns 1 if an echo reply matching (id, seq) has been received since
 * the last call that returned 1 for this exact pair (consumes the match -
 * a second poll for the same (id, seq) after it's already been reported
 * returns 0, so a caller looping on this can't observe one reply twice).
 * Callers (the boot self-test) poll this rather than blocking inside
 * icmp_send_echo_request itself, since the reply arrives asynchronously
 * via the RX IRQ handler. */
int icmp_echo_reply_seen(uint16_t id, uint16_t seq);

/* Called by ip.c for a received ICMP packet. Answers an echo request
 * (type 8) by swapping src/dst and replying with the identical id/seq/
 * payload (type 0) - runs from IRQ context when the request arrived
 * asynchronously, same as arp_handle_packet. Records an echo reply (type
 * 0) for icmp_echo_reply_seen to find. */
void icmp_handle_packet(uint32_t src_ip, const uint8_t *payload, uint16_t len);
