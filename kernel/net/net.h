/* kernel/net/net.h
 *
 * Networking's top-level init and this machine's IP configuration.
 *
 * M27 hardcoded the address, and said so plainly: no DHCP client, out of
 * scope, QEMU's `-netdev user` always hands out 10.0.2.15 and always
 * answers as the gateway at 10.0.2.2, so two constants were a working
 * configuration on the one network this project boots on.
 *
 * M64 makes that a *fallback* rather than the answer. The moment user
 * space can open a socket, "works on the one network we boot on" stops
 * being good enough - a program that resolves a name or asks a time
 * server is a program someone will run somewhere else. So the address,
 * the gateway, the mask and the DNS server are now variables that a DHCP
 * exchange fills in at boot, and the old constants are what they fall
 * back to when nothing answers. Everything above this file was already
 * written against net_local_ip() rather than the macro, which is the only
 * reason this was a small change.
 */
#pragma once

#include <stdint.h>

/* IPv4 addresses as a plain uint32_t, most significant octet first (i.e.
 * 10.0.2.15 == 0x0A00020F) - matches how ip.c reads/writes them straight
 * out of the big-endian on-wire header with no byte-swapping helper
 * needed, since this kernel only ever runs on a little-endian host CPU
 * but never puts these values in a CPU register the wire format has to
 * match endianness with (see ip.c's own header comment). */
#define NET_FALLBACK_IP      0x0A00020Fu /* 10.0.2.15 */
#define NET_FALLBACK_GATEWAY 0x0A000202u /* 10.0.2.2  */
#define NET_FALLBACK_MASK    0xFFFFFF00u /* /24 */
#define NET_FALLBACK_DNS     0x0A000203u /* 10.0.2.3 - slirp's DNS forwarder */

#define NET_BROADCAST_IP     0xFFFFFFFFu
#define NET_LOOPBACK_NET     0x7F000000u /* 127.0.0.0/8 */
#define NET_LOOPBACK_MASK    0xFF000000u

/* Brings the NIC up (rtl8139_init), runs a DHCP exchange if one can be
 * had, and logs the resulting configuration. Returns 1 if a NIC was
 * found, 0 otherwise - see rtl8139_init's own comment on why "not
 * present" isn't a panic here the way it is for every other driver in
 * this kernel. Callers that only care about hardware actually present
 * (the boot self-test) should skip anything depending on the network
 * entirely when this returns 0, the same "installed but untested this
 * boot" degradation kernel.c's keyboard/mouse self-tests already use for
 * present-but-unexercised hardware. */
int net_init(void);

const uint8_t *net_local_mac(void);

uint32_t net_local_ip(void);
uint32_t net_gateway_ip(void);
uint32_t net_subnet_mask(void);
uint32_t net_dns_ip(void);

/* Whether the current configuration came from a DHCP server or from the
 * fallback constants above. The boot self-test asserts the first on a
 * machine with a NIC, because "we fell back" and "we got a lease" look
 * identical from the outside on the one network where the fallback
 * happens to be right - which is exactly how a broken DHCP client would
 * go unnoticed here forever. */
int net_config_is_leased(void);

/* Whether net_init found a NIC at all. SYS_netconf answers -1 without
 * one rather than reporting a configuration no packet could ever use. */
int net_have_nic(void);

/* Installed by dhcp.c once it has an ACK. Kept as a setter rather than
 * dhcp.c reaching into net.c's variables so there is one place that
 * decides what "configured" means. */
void net_set_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns);

/* True if dst is this machine - our own address, anything in 127/8, or
 * the all-ones broadcast we are also entitled to receive. ip.c uses it
 * for both the loopback shortcut on send and the accept check on
 * receive, so the two can never disagree about what "for us" means. */
int net_is_local_ip(uint32_t ip);

/* ---- M67: net_lock ---------------------------------------------------
 *
 * What it protects: the whole of kernel/net - the socket table, every
 * TCB and its send/receive buffers, the ARP cache, the IP loopback queue
 * and the RTL8139's own rx cursor. One lock for the subsystem rather
 * than one per table, because a single inbound segment walks all of them
 * in one go (eth -> ip -> tcp -> the socket's receive buffer) and
 * anything finer would be four acquisitions in a fixed order pretending
 * to be four independent things.
 *
 * Against whom: the NIC's interrupt handler, which has always been able
 * to run at any instant, versus a syscall, which until M67 could not be
 * interrupted at all. `int 0x80` with an interrupt gate meant SYS_recv
 * could walk a socket's datagram queue with a hardware guarantee that
 * rtl8139_irq would not be appending to it half-way through. A trap gate
 * removes that guarantee, and this lock replaces it.
 *
 * Taken at exactly THREE places, all of them boundaries where control
 * enters kernel/net from outside it:
 *
 *   1. eth_receive        - inbound from the wire, in IRQ context
 *   2. tcp_tick           - the 100 ms retransmission clock, its own thread
 *   3. the net syscalls   - dispatched under it in syscall_handler
 *
 * and NOWHERE inside. That is what keeps it a plain spinlock rather than
 * a recursive one: M66's loopback drain re-enters the receive path from
 * inside a send, so a lock taken per-function would deadlock on itself
 * the first time a program talked to its own machine. The rule is
 * "acquire on the way in, never below" - if a new entry point appears,
 * it locks; if a new internal helper appears, it must not.
 *
 * Every net syscall is non-blocking by construction (SYS_connect returns
 * once the SYN is away, SYS_recv returns 0 rather than waiting), so
 * holding this with interrupts off for a whole syscall is bounded by a
 * buffer copy - there is no path under it that calls schedule(). */
void net_lock_acquire(void);
void net_lock_release(void);
