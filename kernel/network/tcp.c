#include "tcp.h"

#include "ip.h"
#include "drivers/kernel_log.h"
#include "library/kernel_library.h"
#include "memory_management/heap.h"
#include "network.h"
#include "wire.h"
#include "scheduler/scheduler.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_HEADER_LENGTH 20

static int sequence_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static int sequence_gt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) > 0; }
static int sequence_geq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

struct tcpcb {
    int in_use;
    tcp_state_t state;

    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    uint32_t snd_una, snd_nxt, snd_wnd, iss;
    uint32_t rcv_nxt, irs;

    uint16_t mss;

    uint8_t send_buffer[TCP_SEND_BUFFER];
    uint32_t send_length;

    uint8_t receive_buffer[TCP_RECEIVE_BUFFER];
    uint32_t receive_length;

    uint32_t cwnd, ssthresh;
    uint32_t dup_acks;
    uint32_t recover;

    int      rtx_pending;
    uint32_t rtx_deadline;
    uint32_t rto;
    int      backoff;
    int      rtt_timing;
    uint32_t rtt_sequence;
    uint32_t rtt_started;
    int32_t  srtt, rttvar;

    uint32_t time_wait_deadline;
    uint32_t connect_deadline;

    int fin_sent;
    uint32_t fin_sequence;
    int peer_fin;
    int connect_failed;
    int reset;
    int sending;

    int released;
    uint32_t linger_deadline;

    struct tcpcb *listener;
    int pending_accept;
};

static struct tcpcb *tcbs[TCP_MAX_TCBS];
static int tcb_count;
static uint32_t tick_count;
static uint32_t isn_counter;

static int debug_drop_remaining;
static int debug_retransmits;

void tcp_debug_drop_next(int segments) { debug_drop_remaining = segments; }
int tcp_debug_retransmits(void) { return debug_retransmits; }

static uint32_t checksum_failures;
uint32_t tcp_checksum_failures(void) { return checksum_failures; }

static uint32_t pseudo_sum(uint32_t source, uint32_t destination, uint16_t seg_length) {
    uint8_t p[12];
    net_write_be32(p + 0, source);
    net_write_be32(p + 4, destination);
    p[8] = 0;
    p[9] = IP_PROTO_TCP;
    net_write_be16(p + 10, seg_length);
    return net_sum16(0, p, sizeof(p));
}

static uint16_t window_of(const struct tcpcb *t) {
    uint32_t free_space = TCP_RECEIVE_BUFFER - t->receive_length;
    return free_space > 0xFFFF ? 0xFFFF : (uint16_t)free_space;
}

static int emit(struct tcpcb *t, uint32_t seq, uint8_t flags,
                const uint8_t *payload, uint16_t payload_length, int with_mss) {
    static uint8_t seg[TCP_HEADER_LENGTH + 4 + TCP_MAX_MSS];
    uint16_t header_length = TCP_HEADER_LENGTH + (with_mss ? 4 : 0);

    if (payload_length > TCP_MAX_MSS) {
        payload_length = TCP_MAX_MSS;
    }

    net_write_be16(seg + 0, t->local_port);
    net_write_be16(seg + 2, t->remote_port);
    net_write_be32(seg + 4, seq);
    net_write_be32(seg + 8, (flags & TCP_ACK) ? t->rcv_nxt : 0);
    seg[12] = (uint8_t)((header_length / 4) << 4);
    seg[13] = flags;
    net_write_be16(seg + 14, window_of(t));
    seg[16] = 0;
    seg[17] = 0;
    net_write_be16(seg + 18, 0);

    if (with_mss) {
        seg[20] = 2;
        seg[21] = 4;
        net_write_be16(seg + 22, TCP_MAX_MSS);
    }
    if (payload_length) {
        k_memcpy(seg + header_length, payload, payload_length);
    }

    uint16_t total = (uint16_t)(header_length + payload_length);
    uint16_t csum = net_fold16(net_sum16(pseudo_sum(t->local_ip, t->remote_ip, total), seg, total));
    net_write_be16(seg + 16, csum);

    if (payload_length > 0 && debug_drop_remaining > 0) {
        debug_drop_remaining--;
        return 0;
    }

    return ip_send_from(t->local_ip, t->remote_ip, IP_PROTO_TCP, seg, total);
}

static void send_reset(uint32_t source_ip, uint32_t destination_ip, const uint8_t *seg, uint16_t length) {
    if (seg[13] & TCP_RST) {
        return;
    }
    kernel_log_puts("[tcp] rst: no connection for a segment to port ");
    kernel_log_put_dec(net_read_be16(seg + 2));
    kernel_log_puts(" from port ");
    kernel_log_put_dec(net_read_be16(seg + 0));
    kernel_log_puts(" flags ");
    kernel_log_put_hex32(seg[13]);
    kernel_log_putc('\n');
    uint8_t out[TCP_HEADER_LENGTH];
    uint16_t data_off = (uint16_t)((seg[12] >> 4) * 4);
    uint32_t their_sequence = net_read_be32(seg + 4);
    uint32_t their_ack = net_read_be32(seg + 8);
    uint16_t seg_length = (uint16_t)(length - data_off);
    if (seg[13] & TCP_SYN) { seg_length++; }
    if (seg[13] & TCP_FIN) { seg_length++; }

    net_write_be16(out + 0, net_read_be16(seg + 2));
    net_write_be16(out + 2, net_read_be16(seg + 0));
    out[12] = (uint8_t)((TCP_HEADER_LENGTH / 4) << 4);
    net_write_be16(out + 14, 0);
    out[16] = 0; out[17] = 0;
    net_write_be16(out + 18, 0);

    if (seg[13] & TCP_ACK) {
        net_write_be32(out + 4, their_ack);
        net_write_be32(out + 8, 0);
        out[13] = TCP_RST;
    } else {
        net_write_be32(out + 4, 0);
        net_write_be32(out + 8, their_sequence + seg_length);
        out[13] = TCP_RST | TCP_ACK;
    }

    uint16_t csum = net_fold16(net_sum16(pseudo_sum(destination_ip, source_ip, TCP_HEADER_LENGTH), out, TCP_HEADER_LENGTH));
    net_write_be16(out + 16, csum);
    ip_send_from(destination_ip, source_ip, IP_PROTO_TCP, out, TCP_HEADER_LENGTH);
}

static void send_ack(struct tcpcb *t) {
    emit(t, t->snd_nxt, TCP_ACK, (const uint8_t *)0, 0, 0);
}

static void arm_rtx(struct tcpcb *t) {
    t->rtx_pending = 1;
    t->rtx_deadline = tick_count + t->rto;
}

static void disarm_rtx(struct tcpcb *t) {
    t->rtx_pending = 0;
    t->backoff = 0;
}

static void note_rtt(struct tcpcb *t, uint32_t measured) {
    int32_t r = (int32_t)measured;
    if (t->srtt == 0) {
        t->srtt = r << 3;
        t->rttvar = r << 1;
    } else {
        int32_t error = r - (t->srtt >> 3);
        t->srtt += error;
        int32_t abs_error = error < 0 ? -error : error;
        t->rttvar += (abs_error - (t->rttvar >> 2));
    }
    uint32_t rto = (uint32_t)((t->srtt >> 3) + (t->rttvar >> 1));
    if (rto < 10) { rto = 10; }
    if (rto > 100) { rto = 100; }
    t->rto = rto;
}

static void try_send(struct tcpcb *t) {
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT &&
        t->state != TCP_FIN_WAIT_1 && t->state != TCP_LAST_ACK &&
        t->state != TCP_CLOSING) {
        return;
    }

    if (t->sending) {
        return;
    }
    t->sending = 1;

    for (;;) {
        uint32_t in_flight = t->snd_nxt - t->snd_una;
        uint32_t usable = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
        if (in_flight >= usable) {
            break;
        }
        uint32_t room = usable - in_flight;
        uint32_t unsent = t->send_length - in_flight;
        if (unsent == 0) {
            break;
        }
        uint32_t n = unsent < room ? unsent : room;
        if (n > t->mss) { n = t->mss; }

        uint8_t flags = TCP_ACK;
        if (n == unsent) { flags |= TCP_PSH; }

        emit(t, t->snd_nxt, flags, t->send_buffer + in_flight, (uint16_t)n, 0);

        if (!t->rtt_timing) {
            t->rtt_timing = 1;
            t->rtt_sequence = t->snd_nxt + n;
            t->rtt_started = tick_count;
        }

        t->snd_nxt += n;
        if (!t->rtx_pending) {
            arm_rtx(t);
        }
    }

    if (t->fin_sent && sequence_geq(t->snd_nxt, t->fin_sequence) && t->snd_nxt == t->fin_sequence) {
        emit(t, t->snd_nxt, TCP_FIN | TCP_ACK, (const uint8_t *)0, 0, 0);
        t->snd_nxt++;
        if (!t->rtx_pending) {
            arm_rtx(t);
        }
    }
    t->sending = 0;
}

static void tcb_dispose(struct tcpcb *t);

static struct tcpcb *reclaim_released_tcb(void) {
    struct tcpcb *lingering = (struct tcpcb *)0;
    for (int i = 0; i < tcb_count; i++) {
        struct tcpcb *t = tcbs[i];
        if (!t->in_use || !t->released) {
            continue;
        }
        if (t->state == TCP_TIME_WAIT) {
            tcb_dispose(t);
            return t;
        }
        if (!lingering || sequence_geq(lingering->linger_deadline, t->linger_deadline)) {
            lingering = t;
        }
    }
    if (lingering) {
        tcp_abort(lingering);
        return lingering;
    }
    return (struct tcpcb *)0;
}

/* A block nobody holds any more is the first thing given back when the
   table is full. RFC 1122 4.2.2.13 lets TIME_WAIT be cut short for a new
   connection, and a released block that is still waiting for its FIN to be
   acknowledged belongs to a program that has already closed it - refusing a
   live program a socket to keep that one would be the wrong way round. */
static struct tcpcb *alloc_tcb(void) {
    struct tcpcb *t = (struct tcpcb *)0;
    for (int i = 0; i < tcb_count && !t; i++) {
        if (!tcbs[i]->in_use) {
            t = tcbs[i];
        }
    }
    if (!t && tcb_count < TCP_MAX_TCBS) {
        t = (struct tcpcb *)kmalloc(sizeof(struct tcpcb));
        if (t) {
            tcbs[tcb_count++] = t;
        }
    }
    if (!t) {
        t = reclaim_released_tcb();
    }
    if (!t) {
        return (struct tcpcb *)0;
    }
    k_memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->state = TCP_CLOSED;
    t->mss = TCP_DEFAULT_MSS;
    t->rto = 10;
    t->cwnd = 2 * TCP_DEFAULT_MSS;
    t->ssthresh = 0xFFFF;
    return t;
}

static void tcb_dispose(struct tcpcb *t) {
    if (!t) {
        return;
    }
    int never_handed_over = t->pending_accept && t->listener;

    t->state = TCP_CLOSED;
    t->remote_port = 0;
    t->listener = (struct tcpcb *)0;
    t->pending_accept = 0;
    t->rtx_pending = 0;
    if (t->released || never_handed_over) {
        t->in_use = 0;
    }
}

void tcp_release(struct tcpcb *t) {
    if (!t || !t->in_use) {
        return;
    }
    t->released = 1;
    if (t->state == TCP_CLOSED) {
        t->in_use = 0;
        return;
    }
    t->linger_deadline = tick_count + 600;
}

static struct tcpcb *find_tcb(uint32_t local_ip, uint16_t local_port,
                              uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < tcb_count; i++) {
        struct tcpcb *t = tcbs[i];
        if (t->in_use && t->state != TCP_LISTEN &&
            t->local_port == local_port && t->remote_port == remote_port &&
            t->remote_ip == remote_ip && t->local_ip == local_ip) {
            return t;
        }
    }
    for (int i = 0; i < tcb_count; i++) {
        struct tcpcb *t = tcbs[i];
        if (t->in_use && t->state == TCP_LISTEN && t->local_port == local_port) {
            return t;
        }
    }
    return (struct tcpcb *)0;
}

static int port_in_use(uint16_t port) {
    for (int i = 0; i < tcb_count; i++) {
        if (tcbs[i]->in_use && tcbs[i]->local_port == port) {
            return 1;
        }
    }
    return 0;
}

#define TCP_EPHEMERAL_FIRST 49152u
#define TCP_EPHEMERAL_LAST  65535u
static uint32_t ephemeral = TCP_EPHEMERAL_FIRST;

static uint16_t pick_ephemeral(void) {
    for (uint32_t i = 0; i <= TCP_EPHEMERAL_LAST - TCP_EPHEMERAL_FIRST; i++) {
        uint16_t p = (uint16_t)ephemeral;
        ephemeral = ephemeral >= TCP_EPHEMERAL_LAST ? TCP_EPHEMERAL_FIRST : ephemeral + 1;
        if (!port_in_use(p)) {
            return p;
        }
    }
    return 0;
}

static uint32_t next_isn(void) {
    isn_counter += 64000;
    return isn_counter + tick_count * 250000u;
}

void tcp_init(void) {
    tcb_count = 0;
    tick_count = 0;
    isn_counter = 0x1EA50000u;
    ephemeral = TCP_EPHEMERAL_FIRST;
    checksum_failures = 0;
}

struct tcpcb *tcp_open(void) {
    return alloc_tcb();
}

int tcp_bind(struct tcpcb *t, uint16_t port) {
    if (!t || !t->in_use || t->local_port) {
        return -1;
    }
    if (port == 0) {
        port = pick_ephemeral();
        if (!port) {
            return -1;
        }
    } else if (port_in_use(port)) {
        return -1;
    }
    t->local_port = port;
    return port;
}

/* M226: SO_REUSEADDR, as Linux's TCP has it. Without it, a port is taken
   while anything holds it - including the connections a server accepted and
   then closed, which sit in TIME_WAIT for two segment lifetimes after the
   server is gone. With it, the port is taken only by a listener, or by a
   block a live socket has bound and not yet connected: a server that
   restarts gets its port back, and two listeners on one port are still
   refused. */
static int port_taken_reusing(uint16_t port) {
    for (int i = 0; i < tcb_count; i++) {
        struct tcpcb *t = tcbs[i];
        if (!t->in_use || t->local_port != port) {
            continue;
        }
        if (t->state == TCP_LISTEN || (t->state == TCP_CLOSED && !t->released)) {
            return 1;
        }
    }
    return 0;
}

int tcp_bind_reusing(struct tcpcb *t, uint16_t port) {
    if (!t || !t->in_use || t->local_port || port == 0) {
        return tcp_bind(t, port);
    }
    if (port_taken_reusing(port)) {
        return -1;
    }
    t->local_port = port;
    return port;
}

int tcp_listen(struct tcpcb *t) {
    if (!t || !t->in_use || !t->local_port || t->state != TCP_CLOSED) {
        return -1;
    }
    t->state = TCP_LISTEN;
    t->local_ip = net_local_ip();
    return 0;
}

int tcp_connect(struct tcpcb *t, uint32_t ip, uint16_t port) {
    if (!t || !t->in_use || t->state != TCP_CLOSED) {
        return -1;
    }
    if (!t->local_port && tcp_bind(t, 0) < 0) {
        return -1;
    }
    t->local_ip = ((ip & NET_LOOPBACK_MASK) == NET_LOOPBACK_NET) ? ip : net_local_ip();
    t->remote_ip = ip;
    t->remote_port = port;
    t->iss = next_isn();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss;
    t->snd_wnd = TCP_DEFAULT_MSS;
    t->state = TCP_SYN_SENT;
    t->connect_deadline = tick_count + 100;

    t->snd_nxt = t->iss + 1;
    arm_rtx(t);
    emit(t, t->iss, TCP_SYN, (const uint8_t *)0, 0, 1);
    return 0;
}

struct tcpcb *tcp_accept(struct tcpcb *listener) {
    if (!listener || listener->state != TCP_LISTEN) {
        return (struct tcpcb *)0;
    }
    for (int i = 0; i < tcb_count; i++) {
        struct tcpcb *t = tcbs[i];
        if (t->in_use && t->listener == listener && t->pending_accept &&
            t->state == TCP_ESTABLISHED) {
            t->pending_accept = 0;
            t->listener = (struct tcpcb *)0;
            return t;
        }
    }
    return (struct tcpcb *)0;
}

int tcp_accept_pending(const struct tcpcb *listener) {
    if (!listener || listener->state != TCP_LISTEN) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < tcb_count; i++) {
        const struct tcpcb *t = tcbs[i];
        if (t->in_use && t->listener == listener && t->pending_accept &&
            t->state == TCP_ESTABLISHED) {
            n++;
        }
    }
    return n;
}

int tcp_send(struct tcpcb *t, const uint8_t *data, uint16_t length) {
    if (!t || !t->in_use || t->fin_sent) {
        return -1;
    }
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) {
        return -1;
    }
    uint32_t room = TCP_SEND_BUFFER - t->send_length;
    uint32_t n = length < room ? length : room;
    if (n) {
        k_memcpy(t->send_buffer + t->send_length, data, n);
        t->send_length += n;
        try_send(t);
    }
    return (int)n;
}

int tcp_receive(struct tcpcb *t, uint8_t *out, uint16_t max) {
    if (!t || !t->in_use) {
        return -1;
    }
    if (t->receive_length == 0) {
        if (t->peer_fin || t->reset || t->state == TCP_CLOSED) {
            return -1;
        }
        return 0;
    }
    uint32_t n = t->receive_length < max ? t->receive_length : max;
    k_memcpy(out, t->receive_buffer, n);
    t->receive_length -= n;
    if (t->receive_length) {
        k_memmove(t->receive_buffer, t->receive_buffer + n, t->receive_length);
    }
    if (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2) {
        send_ack(t);
    }
    return (int)n;
}

int tcp_receive_ended(const struct tcpcb *t) {
    if (!t || !t->in_use) {
        return 1;
    }
    return t->peer_fin || t->reset || (t->state == TCP_CLOSED && t->remote_port != 0);
}

int tcp_peek(const struct tcpcb *t, uint8_t *out, uint16_t max) {
    if (!t || !t->in_use) {
        return -1;
    }
    if (t->receive_length == 0) {
        if (t->peer_fin || t->reset || t->state == TCP_CLOSED) {
            return -1;
        }
        return 0;
    }
    uint32_t n = t->receive_length < max ? t->receive_length : max;
    k_memcpy(out, t->receive_buffer, n);
    return (int)n;
}

int tcp_bytes_available(const struct tcpcb *t) {
    return (t && t->in_use) ? (int)t->receive_length : 0;
}

int tcp_send_space(const struct tcpcb *t) {
    return (t && t->in_use) ? (int)(TCP_SEND_BUFFER - t->send_length) : 0;
}

tcp_state_t tcp_state(const struct tcpcb *t) {
    return (t && t->in_use) ? t->state : TCP_CLOSED;
}

uint16_t tcp_local_port(const struct tcpcb *t) { return t ? t->local_port : 0; }
uint32_t tcp_local_ip(const struct tcpcb *t) { return t ? t->local_ip : 0; }
uint32_t tcp_remote_ip(const struct tcpcb *t) { return t ? t->remote_ip : 0; }
uint16_t tcp_remote_port(const struct tcpcb *t) { return t ? t->remote_port : 0; }

int tcp_connect_settled(const struct tcpcb *t) {
    if (!t || !t->in_use) {
        return 1;
    }
    return t->state != TCP_SYN_SENT;
}

int tcp_connect_failed(const struct tcpcb *t) {
    return !t || !t->in_use || t->connect_failed || t->state == TCP_CLOSED;
}

void tcp_close(struct tcpcb *t) {
    if (!t || !t->in_use) {
        return;
    }
    switch (t->state) {
    case TCP_LISTEN:
        for (int i = 0; i < tcb_count; i++) {
            if (tcbs[i]->in_use && tcbs[i]->listener == t) {
                tcbs[i]->released = 1;
                tcp_abort(tcbs[i]);
            }
        }
        tcb_dispose(t);
        return;
    case TCP_CLOSED:
        tcb_dispose(t);
        return;
    case TCP_SYN_SENT:
        tcb_dispose(t);
        return;
    case TCP_ESTABLISHED:
        t->fin_sent = 1;
        t->fin_sequence = t->snd_una + t->send_length;
        t->state = TCP_FIN_WAIT_1;
        try_send(t);
        return;
    case TCP_CLOSE_WAIT:
        t->fin_sent = 1;
        t->fin_sequence = t->snd_una + t->send_length;
        t->state = TCP_LAST_ACK;
        try_send(t);
        return;
    default:
        return;
    }
}

void tcp_abort(struct tcpcb *t) {
    if (!t || !t->in_use) {
        return;
    }
    if (t->state != TCP_LISTEN && t->state != TCP_CLOSED && t->remote_port) {
        kernel_log_puts("[tcp] rst: aborting local port ");
        kernel_log_put_dec(t->local_port);
        kernel_log_puts(" to port ");
        kernel_log_put_dec(t->remote_port);
        kernel_log_puts(" in state ");
        kernel_log_put_dec((uint32_t)t->state);
        kernel_log_putc('\n');
        emit(t, t->snd_nxt, TCP_RST, (const uint8_t *)0, 0, 0);
    }
    t->released = 1;
    tcb_dispose(t);
}

static void parse_mss(struct tcpcb *t, const uint8_t *seg, uint16_t data_off) {
    uint16_t i = TCP_HEADER_LENGTH;
    while (i + 1 < data_off) {
        uint8_t kind = seg[i];
        if (kind == 0) { break; }
        if (kind == 1) { i++; continue; }
        uint8_t olen = seg[i + 1];
        if (olen < 2 || i + olen > data_off) { break; }
        if (kind == 2 && olen == 4) {
            uint16_t mss = net_read_be16(seg + i + 2);
            if (mss > 0) {
                t->mss = mss < TCP_MAX_MSS ? mss : TCP_MAX_MSS;
                t->cwnd = 2u * t->mss;
            }
        }
        i = (uint16_t)(i + olen);
    }
}

static int process_ack(struct tcpcb *t, uint32_t ack, uint32_t window) {
    int window_moved = window != t->snd_wnd;
    t->snd_wnd = window;

    if (sequence_leq(ack, t->snd_una)) {
        if (ack == t->snd_una && t->send_length > 0 && !window_moved) {
            t->dup_acks++;
            if (t->dup_acks == 3) {
                t->ssthresh = (t->snd_nxt - t->snd_una) / 2;
                if (t->ssthresh < 2u * t->mss) { t->ssthresh = 2u * t->mss; }
                t->recover = t->snd_nxt;
                emit(t, t->snd_una, TCP_ACK | TCP_PSH, t->send_buffer, (uint16_t)(t->send_length < t->mss ? t->send_length : t->mss), 0);
                debug_retransmits++;
                t->cwnd = t->ssthresh + 3u * t->mss;
                arm_rtx(t);
            } else if (t->dup_acks > 3) {
                t->cwnd += t->mss;
                try_send(t);
            }
        }
        return 0;
    }
    if (sequence_gt(ack, t->snd_nxt)) {
        return 0;
    }

    uint32_t acked = ack - t->snd_una;

    uint32_t data_acked = acked;
    if (t->fin_sent && sequence_gt(ack, t->fin_sequence)) {
        data_acked = acked - 1;
    }
    if (data_acked > t->send_length) {
        data_acked = t->send_length;
    }
    if (data_acked) {
        t->send_length -= data_acked;
        if (t->send_length) {
            k_memmove(t->send_buffer, t->send_buffer + data_acked, t->send_length);
        }
    }
    t->snd_una = ack;

    if (t->dup_acks >= 3) {
        t->cwnd = t->ssthresh;
    } else if (t->cwnd < t->ssthresh) {
        t->cwnd += t->mss;
    } else {
        uint32_t inc = (uint32_t)t->mss * t->mss / (t->cwnd ? t->cwnd : 1);
        t->cwnd += inc ? inc : 1;
    }
    t->dup_acks = 0;

    if (t->rtt_timing && sequence_geq(ack, t->rtt_sequence)) {
        note_rtt(t, tick_count - t->rtt_started);
        t->rtt_timing = 0;
    }

    if (t->snd_una == t->snd_nxt) {
        disarm_rtx(t);
    } else {
        arm_rtx(t);
    }
    return 1;
}

static void deliver(struct tcpcb *t, const uint8_t *data, uint16_t length) {
    uint32_t room = TCP_RECEIVE_BUFFER - t->receive_length;
    uint32_t n = length < room ? length : room;
    if (n) {
        k_memcpy(t->receive_buffer + t->receive_length, data, n);
        t->receive_length += n;
        t->rcv_nxt += n;
    }
    if (n < length) {
        kernel_log_puts("[tcp] window: port ");
        kernel_log_put_dec(t->local_port);
        kernel_log_puts(" refused ");
        kernel_log_put_dec(length - n);
        kernel_log_puts(" of ");
        kernel_log_put_dec(length);
        kernel_log_puts(" bytes, buffer ");
        kernel_log_put_dec(t->receive_length);
        kernel_log_putc('\n');
    }
}

static void tcp_handle_packet_locked(uint32_t source_ip, uint32_t destination_ip, const uint8_t *seg, uint16_t length) {
    if (length < TCP_HEADER_LENGTH) {
        return;
    }
    uint16_t data_off = (uint16_t)((seg[12] >> 4) * 4);
    if (data_off < TCP_HEADER_LENGTH || data_off > length) {
        return;
    }
    if (net_fold16(net_sum16(pseudo_sum(source_ip, destination_ip, length), seg, length)) != 0) {
        checksum_failures++;
        if (checksum_failures <= 4 || checksum_failures % 256 == 0) {
            kernel_log_puts("[tcp] checksum: dropped a corrupt segment to port ");
            kernel_log_put_dec(net_read_be16(seg + 2));
            kernel_log_puts(" from port ");
            kernel_log_put_dec(net_read_be16(seg + 0));
            kernel_log_puts(", ");
            kernel_log_put_dec(checksum_failures);
            kernel_log_puts(" since boot\n");
        }
        return;
    }

    uint16_t source_port = net_read_be16(seg + 0);
    uint16_t destination_port = net_read_be16(seg + 2);
    uint32_t seq = net_read_be32(seg + 4);
    uint32_t ack = net_read_be32(seg + 8);
    uint8_t flags = seg[13];
    uint16_t window = net_read_be16(seg + 14);
    const uint8_t *data = seg + data_off;
    uint16_t data_length = (uint16_t)(length - data_off);

    struct tcpcb *t = find_tcb(destination_ip, destination_port, source_ip, source_port);
    if (!t) {
        send_reset(source_ip, destination_ip, seg, length);
        return;
    }

    if (t->state == TCP_LISTEN) {
        if (!(flags & TCP_SYN) || (flags & TCP_ACK)) {
            send_reset(source_ip, destination_ip, seg, length);
            return;
        }
        struct tcpcb *c = alloc_tcb();
        if (!c) {
            send_reset(source_ip, destination_ip, seg, length);
            return;
        }
        c->local_ip = destination_ip;
        c->local_port = destination_port;
        c->remote_ip = source_ip;
        c->remote_port = source_port;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->snd_wnd = window;
        c->iss = next_isn();
        c->snd_una = c->iss;
        c->snd_nxt = c->iss;
        c->listener = t;
        c->pending_accept = 1;
        c->state = TCP_SYN_RECEIVED;
        parse_mss(c, seg, data_off);
        c->snd_nxt = c->iss + 1;
        emit(c, c->iss, TCP_SYN | TCP_ACK, (const uint8_t *)0, 0, 1);
        arm_rtx(c);
        return;
    }

    if (t->state == TCP_SYN_SENT) {
        if (flags & TCP_RST) {
            t->connect_failed = 1;
            tcb_dispose(t);
            return;
        }
        if (!(flags & TCP_SYN) || !(flags & TCP_ACK) || ack != t->snd_nxt) {
            return;
        }
        t->irs = seq;
        t->rcv_nxt = seq + 1;
        t->snd_una = ack;
        t->snd_wnd = window;
        parse_mss(t, seg, data_off);
        t->state = TCP_ESTABLISHED;
        disarm_rtx(t);
        send_ack(t);
        try_send(t);
        return;
    }

    if (flags & TCP_RST) {
        if (seq != t->rcv_nxt) {
            return;
        }
        t->reset = 1;
        if (t->state == TCP_TIME_WAIT || t->state == TCP_LAST_ACK ||
            t->pending_accept) {
            tcb_dispose(t);
        } else {
            t->state = TCP_CLOSED;
        }
        return;
    }

    int carries_no_sequence = (data_length == 0) && !(flags & (TCP_SYN | TCP_FIN));
    if (seq != t->rcv_nxt && !carries_no_sequence) {
        if (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
            t->state == TCP_FIN_WAIT_2 || t->state == TCP_CLOSE_WAIT) {
            send_ack(t);
        }
        return;
    }

    if (flags & TCP_ACK) {
        process_ack(t, ack, window);
        if (t->state == TCP_SYN_RECEIVED && sequence_geq(t->snd_una, t->iss + 1)) {
            t->state = TCP_ESTABLISHED;
        }
    }

    uint32_t fin_at = seq + data_length;
    int fin_now = (flags & TCP_FIN) != 0;

    if (data_length && seq == t->rcv_nxt &&
        (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
         t->state == TCP_FIN_WAIT_2)) {
        deliver(t, data, data_length);
        if (!(fin_now && t->rcv_nxt == fin_at)) {
            send_ack(t);
        }
    }

    if (fin_now && fin_at == t->rcv_nxt) {
        t->rcv_nxt++;
        t->peer_fin = 1;
        send_ack(t);
        switch (t->state) {
        case TCP_ESTABLISHED:
        case TCP_SYN_RECEIVED:
            t->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            t->state = (t->fin_sent && sequence_gt(t->snd_una, t->fin_sequence)) ? TCP_TIME_WAIT : TCP_CLOSING;
            if (t->state == TCP_TIME_WAIT) {
                t->time_wait_deadline = tick_count + 20;
            }
            break;
        case TCP_FIN_WAIT_2:
            t->state = TCP_TIME_WAIT;
            t->time_wait_deadline = tick_count + 20;
            break;
        default:
            break;
        }
        return;
    }

    if (t->fin_sent && sequence_gt(t->snd_una, t->fin_sequence)) {
        switch (t->state) {
        case TCP_FIN_WAIT_1:
            t->state = TCP_FIN_WAIT_2;
            break;
        case TCP_CLOSING:
            t->state = TCP_TIME_WAIT;
            t->time_wait_deadline = tick_count + 20;
            break;
        case TCP_LAST_ACK:
            tcb_dispose(t);
            break;
        default:
            break;
        }
        return;
    }

    try_send(t);
}

void tcp_handle_packet(uint32_t source_ip, uint32_t destination_ip, const uint8_t *seg, uint16_t length) {
    tcp_handle_packet_locked(source_ip, destination_ip, seg, length);
    scheduler_wake_all(SCHEDULER_POLL_CHAN);

}

void tcp_tick(void) {
    net_lock_acquire();
    tick_count++;

    for (int i = 0; i < tcb_count; i++) {
        struct tcpcb *t = tcbs[i];
        if (!t->in_use) {
            continue;
        }

        if (t->released && t->linger_deadline && sequence_geq(tick_count, t->linger_deadline)) {
            tcb_dispose(t);
            continue;
        }

        if (t->state == TCP_TIME_WAIT) {
            if (sequence_geq(tick_count, t->time_wait_deadline)) {
                tcb_dispose(t);
            }
            continue;
        }

        if (t->state == TCP_SYN_SENT && sequence_geq(tick_count, t->connect_deadline)) {
            t->connect_failed = 1;
            tcb_dispose(t);
            continue;
        }

        if (t->rtx_pending && sequence_geq(tick_count, t->rtx_deadline)) {
            t->ssthresh = (t->snd_nxt - t->snd_una) / 2;
            if (t->ssthresh < 2u * t->mss) { t->ssthresh = 2u * t->mss; }
            t->cwnd = t->mss;
            t->dup_acks = 0;

            t->backoff++;
            if (t->backoff > 6) {
                t->connect_failed = 1;
                t->reset = 1;
                tcb_dispose(t);
                continue;
            }
            t->rto *= 2;
            if (t->rto > 600) { t->rto = 600; }
            t->rtt_timing = 0;

            debug_retransmits++;
            kernel_log_puts("[tcp] rto: port ");
            kernel_log_put_dec(t->local_port);
            kernel_log_puts(" backoff ");
            kernel_log_put_dec(t->backoff);
            kernel_log_puts(" in flight ");
            kernel_log_put_dec(t->snd_nxt - t->snd_una);
            kernel_log_puts(" buffered ");
            kernel_log_put_dec(t->send_length);
            kernel_log_puts(" snd_wnd ");
            kernel_log_put_dec(t->snd_wnd);
            kernel_log_putc('\n');

            if (t->send_length) {
                uint32_t n = t->send_length < t->mss ? t->send_length : t->mss;
                emit(t, t->snd_una, TCP_ACK | TCP_PSH, t->send_buffer, (uint16_t)n, 0);
            } else if (t->state == TCP_SYN_SENT) {
                emit(t, t->iss, TCP_SYN, (const uint8_t *)0, 0, 1);
            } else if (t->state == TCP_SYN_RECEIVED) {
                emit(t, t->iss, TCP_SYN | TCP_ACK, (const uint8_t *)0, 0, 1);
            } else if (t->fin_sent) {
                emit(t, t->fin_sequence, TCP_FIN | TCP_ACK, (const uint8_t *)0, 0, 0);
            }
            t->rtx_deadline = tick_count + t->rto;
        }
    }
    net_lock_release();
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
}
