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

void socket_ref(struct socket *s);
void socket_unref(struct socket *s);

int socket_bind(struct socket *s, uint16_t port);

int socket_sendto(struct socket *s, uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, uint16_t len);

int socket_recvfrom(struct socket *s, uint8_t *out, uint16_t max,
                    uint32_t *src_ip_out, uint16_t *src_port_out);

int socket_pending(const struct socket *s);

void socket_deliver(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                    const uint8_t *data, uint16_t len);

typedef void (*socket_raw_handler_t)(uint32_t src_ip, uint16_t src_port,
                                     const uint8_t *data, uint16_t len);
void socket_set_raw_handler(uint16_t port, socket_raw_handler_t handler);
