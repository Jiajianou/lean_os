/* kernel/net/net.h
 *
 * Networking stretch goal's top-level init and the one piece of config
 * every layer below needs: this machine's own IP. There's no DHCP client
 * (out of scope - "driver + ARP + ICMP" per the stretch goal, not a full
 * stack), so the address is a hardcoded static one instead: QEMU's
 * usermode networking backend (`-netdev user`, what tools/run-qemu.sh and
 * tools/qemu-serial-test.sh attach) always hands out 10.0.2.15 on its
 * built-in DHCP server and always answers as the gateway at 10.0.2.2 -
 * using those same two addresses as constants here means the kernel
 * doesn't need a DHCP client to end up with a working configuration
 * against the one network this project actually boots on.
 */
#pragma once

#include <stdint.h>

/* IPv4 addresses as a plain uint32_t, most significant octet first (i.e.
 * 10.0.2.15 == 0x0A00020F) - matches how ip.c reads/writes them straight
 * out of the big-endian on-wire header with no byte-swapping helper
 * needed, since this kernel only ever runs on a little-endian host CPU
 * but never puts these values in a CPU register the wire format has to
 * match endianness with (see ip.c's own header comment). */
#define NET_LOCAL_IP   0x0A00020Fu /* 10.0.2.15 */
#define NET_GATEWAY_IP 0x0A000202u /* 10.0.2.2  */
#define NET_SUBNET_MASK 0xFFFFFF00u /* /24 */

/* Brings the NIC up (rtl8139_init) and logs the resulting configuration.
 * Panics if no NIC is found - see rtl8139_init's own comment on why that
 * matches every other driver in this kernel. */
void net_init(void);

const uint8_t *net_local_mac(void);
static inline uint32_t net_local_ip(void) { return NET_LOCAL_IP; }
