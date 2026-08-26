/* kernel/net/udp.h
 *
 * M64: UDP, the transport this stack was always one layer short of.
 *
 * There is very little to it, which is the point - a length, two ports
 * and an optional checksum over a pseudo-header - and it is what turns
 * "the kernel can ping the gateway" into "a program can talk to a
 * server". The stack above it is kernel/net/socket.h; the only two
 * things in this kernel that send a datagram without going through a
 * socket are the DHCP client (which runs before there is an address to
 * bind one to) and the boot self-test.
 *
 * The checksum is computed on send and *verified* on receive rather than
 * ignored, because the one thing a from-scratch stack most easily gets
 * wrong is the pseudo-header, and a wrong checksum that nobody checks is
 * a bug that only ever shows up on somebody else's machine.
 */
#pragma once

#include <stdint.h>

#define UDP_HEADER_LEN 8
#define UDP_MAX_PAYLOAD 1472 /* 1500 MTU - 20 IP - 8 UDP */

/* Sends one datagram. Returns 0, or -1 if the payload is too large or
 * the next hop could not be resolved. Never panics: everything above it
 * is reachable from a syscall, and M52's rule is that user space does
 * not get to stop the machine. */
int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const uint8_t *payload, uint16_t payload_len);

/* The same, with the source address stated - see ip_send_from. The DHCP
 * client is the only caller, and the checksum has to be computed over
 * the address that ends up in the header, which is why this exists at
 * this layer too rather than only below it. */
int udp_send_from(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                  const uint8_t *payload, uint16_t payload_len);

/* Called by ip.c for IP_PROTO_UDP. Drops anything whose length or
 * checksum does not check out, then hands the payload to the socket
 * layer - which is the only consumer, and drops it in turn if no socket
 * is bound to the port. */
void udp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *payload, uint16_t len);
