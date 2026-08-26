/* kernel/net/socket.h
 *
 * M64: UDP sockets, as entries in the fd table M59 made real.
 *
 * The stretch-goal entry that asked for this put the problem exactly:
 * M27 shipped Ethernet, ARP, IPv4 and ICMP, and for the thirty-six
 * milestones since, the only thing that has ever used any of it is one
 * boot self-test pinging the gateway. `sock` appeared nowhere in
 * syscall.h. This is the file that changes that, and almost all of its
 * design was already decided:
 *
 *   - It is an fd, not a new handle namespace, because fd_slot_t is
 *     already a tagged union with refcounting, inheritance across spawn
 *     and a working SYS_close. A socket that was its own kind of handle
 *     would need all four again.
 *   - Received datagrams queue in the kernel, because they arrive in the
 *     NIC's IRQ handler and the program that wants them is asleep. The
 *     queue is small and bounded and drops the *newest* on overflow,
 *     which is what UDP is allowed to do and what a program that stops
 *     reading deserves.
 *   - Each queued datagram keeps its source address and port, because
 *     that is the entire difference between recv and recvfrom, and a
 *     client that cannot tell who answered cannot check that the right
 *     server did.
 */
#pragma once

#include <stdint.h>

#include "tcp.h"
#include "udp.h"

#define MAX_SOCKETS 32
#define SOCKET_QUEUE_DEPTH 8

/* The largest datagram a socket will hold. Deliberately smaller than
 * UDP_MAX_PAYLOAD: MAX_SOCKETS * SOCKET_QUEUE_DEPTH of these is kernel
 * BSS that exists whether or not anything ever opens a socket, and 512
 * bytes covers DNS, SNTP, syslog and every other datagram protocol worth
 * having here. A larger one is truncated rather than dropped, and
 * recvfrom says so by returning the length it kept. */
#define SOCKET_MAX_DATAGRAM 512

/* M66: two kinds now. M64's own comment argued against a `type`
 * parameter - "three parameters that only ever take one value each are
 * three ways to be wrong about an API that has no choices in it" - and
 * that argument was correct for as long as its premise held. TCP is the
 * milestone that ends it: there is a real choice now, so there is a real
 * parameter, and SOCK_DGRAM is 0 so every call written before this one
 * still means what it meant. */
#define SOCK_DGRAM  0
#define SOCK_STREAM 1

struct socket;

void socket_init(void);

/* Allocates an unbound socket, or NULL if the table is full. Starts with
 * one reference, which the fd slot takes ownership of. */
struct socket *socket_alloc(int type);

/* SOCK_DGRAM or SOCK_STREAM. */
int socket_type(const struct socket *s);

/* The TCP control block behind a stream socket, or NULL. The syscall
 * layer calls tcp.h directly through this rather than having socket.c
 * mirror every one of TCP's calls - a wrapper that adds nothing but a
 * null check is a layer that only exists to be maintained. */
struct tcpcb *socket_tcb(struct socket *s);

/* M66: stream sockets. bind/listen/connect/accept sit here rather than
 * in tcp.c only because accept has to produce a *socket* the fd table
 * can hold, and tcp.c has no business knowing what an fd is. */
int socket_listen(struct socket *s);
int socket_connect(struct socket *s, uint32_t ip, uint16_t port);
struct socket *socket_accept(struct socket *s);

void socket_ref(struct socket *s);
void socket_unref(struct socket *s);

/* Binds to a port, or to an ephemeral one when `port` is 0 - which is
 * what a client wants and what makes "I only send" a one-call setup.
 * Returns the bound port, or -1 if the port is taken or the socket is
 * already bound. */
int socket_bind(struct socket *s, uint16_t port);

/* Sends, binding an ephemeral port first if the socket has none, so a
 * program that only ever calls sendto still has a source port replies
 * can come back to. Returns the byte count sent, or -1. */
int socket_sendto(struct socket *s, uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, uint16_t len);

/* Dequeues one datagram, copying at most `max` bytes and filling in the
 * sender. Returns the number of bytes copied, or -1 if nothing is
 * queued - this never blocks, because a blocking read on a socket would
 * need the same wait-queue machinery pipes have and nothing here has
 * asked for one; SYS_recvfrom's own comment carries the deal. */
int socket_recvfrom(struct socket *s, uint8_t *out, uint16_t max,
                    uint32_t *src_ip_out, uint16_t *src_port_out);

/* How many datagrams are waiting - the poll SYS_pipe_poll is for pipes,
 * and the reason a non-blocking recvfrom is usable rather than a busy
 * spin with no way to tell "nothing yet" from "nothing ever". */
int socket_pending(const struct socket *s);

/* Called by udp.c from the NIC's IRQ handler for every accepted
 * datagram. Silently drops one addressed to a port nothing is bound to,
 * which is UDP's own answer and cheaper than the ICMP port-unreachable
 * this stack has no reason to send. */
void socket_deliver(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                    const uint8_t *data, uint16_t len);

/* A kernel-side consumer of one port, for the one thing that needs a
 * datagram before user space exists: the DHCP client, which runs before
 * there is an address to bind a socket to. Checked *before* the socket
 * table, so a program cannot bind port 68 and eat a boot-time reply.
 * Called from the NIC's IRQ handler, so a handler must do nothing but
 * copy and set a flag. Pass NULL to unregister. */
typedef void (*socket_raw_handler_t)(uint32_t src_ip, uint16_t src_port,
                                     const uint8_t *data, uint16_t len);
void socket_set_raw_handler(uint16_t port, socket_raw_handler_t handler);
