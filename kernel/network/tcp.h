#pragma once

#include <stdint.h>

typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

#define TCP_SEND_BUFFER   4096
#define TCP_RECEIVE_BUFFER   4096
#define TCP_MAX_TCBS   16
#define TCP_DEFAULT_MSS 536
#define TCP_MAX_MSS    1460

#define TCP_TICK_MS 100

struct tcpcb;

void tcp_init(void);

void tcp_tick(void);

void tcp_handle_packet(uint32_t source_ip, uint32_t destination_ip, const uint8_t *segment, uint16_t length);

struct tcpcb *tcp_open(void);
void tcp_close(struct tcpcb *tcb);

void tcp_release(struct tcpcb *tcb);
void tcp_abort(struct tcpcb *tcb);

int tcp_bind(struct tcpcb *tcb, uint16_t port);
int tcp_listen(struct tcpcb *tcb);
int tcp_connect(struct tcpcb *tcb, uint32_t ip, uint16_t port);

struct tcpcb *tcp_accept(struct tcpcb *listener);

int tcp_accept_pending(const struct tcpcb *listener);

int tcp_send(struct tcpcb *tcb, const uint8_t *data, uint16_t length);

int tcp_receive(struct tcpcb *tcb, uint8_t *out, uint16_t max);

int tcp_bytes_available(const struct tcpcb *tcb);
int tcp_send_space(const struct tcpcb *tcb);
tcp_state_t tcp_state(const struct tcpcb *tcb);
uint16_t tcp_local_port(const struct tcpcb *tcb);
uint32_t tcp_local_ip(const struct tcpcb *tcb);
uint32_t tcp_remote_ip(const struct tcpcb *tcb);
uint16_t tcp_remote_port(const struct tcpcb *tcb);

int tcp_connect_settled(const struct tcpcb *tcb);
int tcp_connect_failed(const struct tcpcb *tcb);

void tcp_debug_drop_next(int data_segments);
int tcp_debug_retransmits(void);

uint32_t tcp_checksum_failures(void);
