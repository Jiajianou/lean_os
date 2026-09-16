#pragma once

#include <stdint.h>

#include "tcp.h"
#include "udp.h"

#define MAX_SOCKETS 32
#define SOCKET_QUEUE_DEPTH 8

#define SOCKET_MAX_DATAGRAM 512

#define SOCK_DGRAM  0
#define SOCK_STREAM 1

struct socket;

void socket_init(void);

struct socket *socket_alloc(int type);


struct tcpcb *socket_tcb(struct socket *s);

int socket_listen(struct socket *s);
int socket_connect(struct socket *s, uint32_t ip, uint16_t port);
struct socket *socket_accept(struct socket *s);

void socket_reference(struct socket *s);
void socket_unref(struct socket *s);

int socket_bind(struct socket *s, uint16_t port);

/* The port this socket is actually on, which for a bind to port zero is
   the one the kernel chose rather than the zero that was asked for. */
uint16_t socket_local_port(const struct socket *s);

/* And which address, which for a connection over the loopback is the
   loopback rather than this machine's own. */
uint32_t socket_local_ip(const struct socket *s);

int socket_sendto(struct socket *s, uint32_t destination_ip, uint16_t destination_port,
                  const uint8_t *data, uint16_t length);

int socket_recvfrom(struct socket *s, uint8_t *out, uint16_t max,
                    uint32_t *source_ip_out, uint16_t *source_port_out);

int socket_pending(const struct socket *s);

void socket_deliver(uint16_t destination_port, uint32_t source_ip, uint16_t source_port,
                    const uint8_t *data, uint16_t length);

typedef void (*socket_raw_handler_t)(uint32_t source_ip, uint16_t source_port,
                                     const uint8_t *data, uint16_t length);
void socket_set_raw_handler(uint16_t port, socket_raw_handler_t handler);
