/* kernel/net/ethernet.h
 *
 * Ethernet II framing: builds/parses the 14-byte header (dst MAC, src MAC,
 * EtherType) around whatever ARP/IP payload the layers above hand down,
 * and dispatches a received frame to the right one by EtherType. The
 * only thing between the NIC driver (kernel/drivers/rtl8139.c) and the
 * rest of the stack.
 */
#pragma once

#include <stdint.h>

#define ETH_ADDR_LEN    6
#define ETH_HEADER_LEN  14
#define ETH_MIN_FRAME   60 /* excluding the 4-byte CRC the NIC appends itself */

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806

extern const uint8_t eth_broadcast_mac[ETH_ADDR_LEN];

/* Builds dst/src/ethertype header + payload (zero-padded up to
 * ETH_MIN_FRAME if payload is short) and hands the result to
 * rtl8139_send. payload_len must leave room under RTL8139_MAX_FRAME once
 * the 14-byte header is added. */
void eth_send(const uint8_t dst_mac[ETH_ADDR_LEN], uint16_t ethertype, const uint8_t *payload, uint16_t payload_len);

/* Called by rtl8139.c's IRQ handler for every frame the NIC receives.
 * Parses the header and dispatches the payload to arp_handle_packet or
 * ip_handle_packet by EtherType; anything else is silently dropped, same
 * as a real NIC driver with no other protocols registered. */
void eth_receive(const uint8_t *frame, uint16_t len);
