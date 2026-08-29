#include "tcp.h"

#include "ip.h"
#include "lib/libk.h"
#include "net.h"

/* ---- wire helpers ----------------------------------------------------
 *
 * Bytes, not a packed struct, for the reason ip.c's header comment gives
 * and this file feels more strongly about: a TCP header has a 4-bit
 * offset and nine 1-bit flags packed into two bytes, and a struct
 * overlay of that on a little-endian machine is a bitfield layout the
 * standard does not pin down. */
static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_HEADER_LEN 20

/* Sequence-space comparison. Every one of these is a subtraction cast to
 * int32_t rather than a comparison of the numbers themselves, because
 * sequence numbers wrap at 2^32 and `a < b` is wrong for exactly the
 * connection that has been running long enough to matter. RFC 793
 * section 3.3. */
static int seq_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static int seq_gt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) > 0; }
static int seq_geq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

struct tcpcb {
    int in_use;
    tcp_state_t state;

    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    /* Send sequence space (RFC 793 3.2). snd_una is the oldest byte not
     * yet acknowledged, snd_nxt the next to send, snd_wnd what the peer
     * says it will take. */
    uint32_t snd_una, snd_nxt, snd_wnd, iss;
    /* Receive sequence space. rcv_nxt is what we expect next and what we
     * acknowledge; rcv_wnd is what is left in the receive buffer. */
    uint32_t rcv_nxt, irs;

    uint16_t mss;

    /* Send buffer, a plain array with snd_una's byte at index 0 - the
     * simplest thing that can retransmit, since what has to be resent is
     * exactly "from snd_una". Compacted on ACK rather than being a ring,
     * because a ring whose base is a sequence number is where this kind
     * of code goes wrong and 4 KiB of memmove per ACK is not a cost
     * anything here can measure. */
    uint8_t send_buf[TCP_SEND_BUF];
    uint32_t send_len;

    uint8_t recv_buf[TCP_RECV_BUF];
    uint32_t recv_len;

    /* Congestion control (RFC 5681). cwnd and ssthresh in bytes. */
    uint32_t cwnd, ssthresh;
    uint32_t dup_acks;
    uint32_t recover; /* fast-recovery high-water mark */

    /* Retransmission. rto in ticks; srtt/rttvar in ticks, scaled the way
     * RFC 6298 scales them (srtt in 1/8, rttvar in 1/4) so the estimator
     * needs no division beyond shifts. */
    int      rtx_pending;
    uint32_t rtx_deadline;
    uint32_t rto;
    int      backoff;
    int      rtt_timing;      /* a measurement is in flight */
    uint32_t rtt_seq;         /* ...of the segment starting here */
    uint32_t rtt_started;     /* ...sent at this tick */
    int32_t  srtt, rttvar;

    /* Timers, in ticks. */
    uint32_t time_wait_deadline;
    uint32_t connect_deadline;

    int fin_sent;        /* our FIN is in the send sequence space */
    uint32_t fin_seq;    /* ...at this sequence number */
    int peer_fin;        /* the peer's FIN has been received and acknowledged */
    int connect_failed;
    int reset;           /* the connection was reset - recv() reports end of stream */

    /* M66: ownership. A control block outlives the socket that owns it -
     * TIME_WAIT is the whole point of the state - and it also has to
     * survive the socket outliving *it*, which is what a connection
     * refused by an RST does: the state machine is finished before the
     * program has called close.
     *
     * So tcp.c never frees a block a socket still points at. The state
     * machine marks it CLOSED; `released` says the socket has let go;
     * and the slot is reclaimed only when both are true. Without this,
     * a `connect` refused on a loopback port completed its whole life
     * inside the syscall that started it, the slot went back on the free
     * list, the next connection took it, and the program's eventual
     * `close` tore down somebody else's connection. */
    int released;
    uint32_t linger_deadline;

    /* A listening tcb owns the connections that completed on it until
     * accept() takes them. A backlog of four: this is a desktop, and a
     * fifth simultaneous half-open connection to a program here means
     * something other than load. */
    struct tcpcb *listener;      /* the tcb this one was accepted onto, while pending */
    int pending_accept;
};

static struct tcpcb tcbs[TCP_MAX_TCBS];
static uint32_t tick_count;
static uint32_t isn_counter;

/* Self-test hooks - see tcp.h. */
static int debug_drop_remaining;
static int debug_retransmits;

void tcp_debug_drop_next(int segments) { debug_drop_remaining = segments; }
int tcp_debug_retransmits(void) { return debug_retransmits; }

/* ---- checksum --------------------------------------------------------
 *
 * The same pseudo-header UDP uses, with TCP's protocol number and the
 * whole segment as its length. */
static uint32_t sum16(uint32_t sum, const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    }
    if (len & 1) {
        sum += (uint32_t)data[len - 1] << 8;
    }
    return sum;
}

static uint16_t fold(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint16_t seg_len) {
    uint8_t p[12];
    write_be32(p + 0, src);
    write_be32(p + 4, dst);
    p[8] = 0;
    p[9] = IP_PROTO_TCP;
    write_be16(p + 10, seg_len);
    return sum16(0, p, sizeof(p));
}

/* ---- sending ---------------------------------------------------------- */

/* How much of the receive buffer is free - what we advertise. */
static uint16_t window_of(const struct tcpcb *t) {
    uint32_t free_space = TCP_RECV_BUF - t->recv_len;
    return free_space > 0xFFFF ? 0xFFFF : (uint16_t)free_space;
}

/* Builds and sends one segment. `payload`/`payload_len` come from the
 * send buffer; `seq` is where they start. Returns 0, or -1 if the send
 * failed below us - which for TCP is not fatal, because the
 * retransmission timer is exactly the machinery that handles a segment
 * that did not arrive, and a send that never left is a special case of
 * that. */
static int emit(struct tcpcb *t, uint32_t seq, uint8_t flags,
                const uint8_t *payload, uint16_t payload_len, int with_mss) {
    static uint8_t seg[TCP_HEADER_LEN + 4 + TCP_MAX_MSS];
    uint16_t hdr_len = TCP_HEADER_LEN + (with_mss ? 4 : 0);

    if (payload_len > TCP_MAX_MSS) {
        payload_len = TCP_MAX_MSS;
    }

    write_be16(seg + 0, t->local_port);
    write_be16(seg + 2, t->remote_port);
    write_be32(seg + 4, seq);
    write_be32(seg + 8, (flags & TCP_ACK) ? t->rcv_nxt : 0);
    seg[12] = (uint8_t)((hdr_len / 4) << 4);
    seg[13] = flags;
    write_be16(seg + 14, window_of(t));
    seg[16] = 0; /* checksum */
    seg[17] = 0;
    write_be16(seg + 18, 0); /* urgent pointer */

    if (with_mss) {
        /* Kind 2, length 4, then the value - and nothing else, so the
         * options field is already a multiple of four and needs no
         * padding. */
        seg[20] = 2;
        seg[21] = 4;
        write_be16(seg + 22, TCP_MAX_MSS);
    }
    if (payload_len) {
        k_memcpy(seg + hdr_len, payload, payload_len);
    }

    uint16_t total = (uint16_t)(hdr_len + payload_len);
    uint16_t csum = fold(sum16(pseudo_sum(t->local_ip, t->remote_ip, total), seg, total));
    write_be16(seg + 16, csum);

    /* The self-test's loss injection. Two things about where it is:
     *
     * It is *after* the segment is fully built and the caller's sequence
     * numbers have advanced, so what the stack believes about this
     * segment is exactly what it would believe if the wire had eaten it.
     * A drop earlier than this would be a different and much easier bug
     * to survive.
     *
     * And it only counts segments carrying data. Dropping a SYN tests
     * connection-establishment retransmission, which is real but is not
     * the path worth proving - and it would push the handshake out past
     * the deadline the test waits on, so the interesting failure would
     * be masked by a boring one. Data segments put the drop squarely in
     * go-back-N. */
    if (payload_len > 0 && debug_drop_remaining > 0) {
        debug_drop_remaining--;
        return 0;
    }

    return ip_send_from(t->local_ip, t->remote_ip, IP_PROTO_TCP, seg, total);
}

/* A bare RST for a segment that arrived for a connection that does not
 * exist. Built from the offending segment rather than from a tcb,
 * because there is no tcb - which is the whole point. RFC 793 3.4. */
static void send_reset(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg, uint16_t len) {
    if (seg[13] & TCP_RST) {
        return; /* never reset a reset - that is how two machines shout at each other forever */
    }
    uint8_t out[TCP_HEADER_LEN];
    uint16_t data_off = (uint16_t)((seg[12] >> 4) * 4);
    uint32_t their_seq = read_be32(seg + 4);
    uint32_t their_ack = read_be32(seg + 8);
    uint16_t seg_len = (uint16_t)(len - data_off);
    if (seg[13] & TCP_SYN) { seg_len++; }
    if (seg[13] & TCP_FIN) { seg_len++; }

    write_be16(out + 0, read_be16(seg + 2)); /* their destination is our source */
    write_be16(out + 2, read_be16(seg + 0));
    out[12] = (uint8_t)((TCP_HEADER_LEN / 4) << 4);
    write_be16(out + 14, 0);
    out[16] = 0; out[17] = 0;
    write_be16(out + 18, 0);

    if (seg[13] & TCP_ACK) {
        /* They told us a sequence number to use, so use it and do not
         * acknowledge anything. */
        write_be32(out + 4, their_ack);
        write_be32(out + 8, 0);
        out[13] = TCP_RST;
    } else {
        write_be32(out + 4, 0);
        write_be32(out + 8, their_seq + seg_len);
        out[13] = TCP_RST | TCP_ACK;
    }

    uint16_t csum = fold(sum16(pseudo_sum(dst_ip, src_ip, TCP_HEADER_LEN), out, TCP_HEADER_LEN));
    write_be16(out + 16, csum);
    ip_send_from(dst_ip, src_ip, IP_PROTO_TCP, out, TCP_HEADER_LEN);
}

static void send_ack(struct tcpcb *t) {
    emit(t, t->snd_nxt, TCP_ACK, (const uint8_t *)0, 0, 0);
}

/* ---- the retransmission timer ---------------------------------------- */

static void arm_rtx(struct tcpcb *t) {
    t->rtx_pending = 1;
    t->rtx_deadline = tick_count + t->rto;
}

static void disarm_rtx(struct tcpcb *t) {
    t->rtx_pending = 0;
    t->backoff = 0;
}

/* RFC 6298's estimator, in ticks. The first measurement seeds it; every
 * one after smooths. rttvar is weighted four times as heavily as srtt in
 * the resulting timeout, which is the part of this algorithm that
 * matters - a timeout derived from the mean alone retransmits constantly
 * on a link whose delay varies. */
static void note_rtt(struct tcpcb *t, uint32_t measured) {
    int32_t r = (int32_t)measured;
    if (t->srtt == 0) {
        t->srtt = r << 3;
        t->rttvar = r << 1;
    } else {
        int32_t err = r - (t->srtt >> 3);
        t->srtt += err;                                   /* srtt += (R - srtt)/8, pre-scaled */
        int32_t abs_err = err < 0 ? -err : err;
        t->rttvar += (abs_err - (t->rttvar >> 2));        /* rttvar += (|err| - rttvar)/4 */
    }
    uint32_t rto = (uint32_t)((t->srtt >> 3) + (t->rttvar >> 1));
    /* One tick is 100 ms, so the RFC's 1-second floor is 10. Clamped up
     * as well as down: a connection whose peer has gone away should give
     * up in tens of seconds, not minutes. */
    if (rto < 10) { rto = 10; }
    if (rto > 100) { rto = 100; }
    t->rto = rto;
}

/* ---- the send buffer and the sending decision ------------------------- */

/* Sends whatever the windows allow, one MSS at a time. Called after
 * anything that could have opened a window: new data queued, an ACK
 * received, a retransmission timeout. */
static void try_send(struct tcpcb *t) {
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT &&
        t->state != TCP_FIN_WAIT_1 && t->state != TCP_LAST_ACK &&
        t->state != TCP_CLOSING) {
        return;
    }

    for (;;) {
        uint32_t in_flight = t->snd_nxt - t->snd_una;
        /* The sender's window is the smaller of what the peer will take
         * and what congestion control allows - RFC 5681's central
         * sentence, and the reason cwnd exists at all. */
        uint32_t usable = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
        if (in_flight >= usable) {
            break;
        }
        uint32_t room = usable - in_flight;
        uint32_t unsent = t->send_len - in_flight;
        if (unsent == 0) {
            break;
        }
        uint32_t n = unsent < room ? unsent : room;
        if (n > t->mss) { n = t->mss; }

        /* PSH on the last segment we have to give, which is what tells a
         * peer's application there is nothing more coming right now. */
        uint8_t flags = TCP_ACK;
        if (n == unsent) { flags |= TCP_PSH; }

        emit(t, t->snd_nxt, flags, t->send_buf + in_flight, (uint16_t)n, 0);

        /* One measurement at a time, and never of a retransmitted
         * segment - Karn's algorithm, and the reason `rtt_timing` is a
         * flag rather than a per-segment timestamp. */
        if (!t->rtt_timing) {
            t->rtt_timing = 1;
            t->rtt_seq = t->snd_nxt + n;
            t->rtt_started = tick_count;
        }

        t->snd_nxt += n;
        if (!t->rtx_pending) {
            arm_rtx(t);
        }
    }

    /* The FIN goes out once everything before it has been sent. */
    if (t->fin_sent && seq_geq(t->snd_nxt, t->fin_seq) && t->snd_nxt == t->fin_seq) {
        emit(t, t->snd_nxt, TCP_FIN | TCP_ACK, (const uint8_t *)0, 0, 0);
        t->snd_nxt++;
        if (!t->rtx_pending) {
            arm_rtx(t);
        }
    }
}

/* ---- lookup and allocation -------------------------------------------- */

static struct tcpcb *alloc_tcb(void) {
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        if (!tcbs[i].in_use) {
            k_memset(&tcbs[i], 0, sizeof(tcbs[i]));
            tcbs[i].in_use = 1;
            tcbs[i].state = TCP_CLOSED;
            tcbs[i].mss = TCP_DEFAULT_MSS;
            tcbs[i].rto = 10;              /* RFC 6298's 1 second, before any measurement */
            tcbs[i].cwnd = 2 * TCP_DEFAULT_MSS;
            tcbs[i].ssthresh = 0xFFFF;
            return &tcbs[i];
        }
    }
    return (struct tcpcb *)0;
}

/* The connection is over as far as TCP is concerned. The slot goes back
 * only if nothing above still holds a pointer to it - see `released`. */
static void tcb_dispose(struct tcpcb *t) {
    if (!t) {
        return;
    }
    t->state = TCP_CLOSED;
    /* Cleared so no arriving segment can match this four-tuple again -
     * a block waiting to be released must not answer for a connection
     * that no longer exists. */
    t->remote_port = 0;
    t->listener = (struct tcpcb *)0;
    t->pending_accept = 0;
    t->rtx_pending = 0;
    if (t->released) {
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
    /* A connection whose peer stops answering mid-shutdown - FIN_WAIT_2
     * is the classic one - would otherwise hold this slot forever.
     * Sixty seconds, which is longer than any close this stack takes and
     * short enough that sixteen slots cannot be exhausted by neglect. */
    t->linger_deadline = tick_count + 600;
}

/* The four-tuple first, then a listener on the port. That order is the
 * whole of TCP demultiplexing: an established connection must win over
 * the listening socket that created it. */
static struct tcpcb *find_tcb(uint32_t local_ip, uint16_t local_port,
                              uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        struct tcpcb *t = &tcbs[i];
        if (t->in_use && t->state != TCP_LISTEN &&
            t->local_port == local_port && t->remote_port == remote_port &&
            t->remote_ip == remote_ip && t->local_ip == local_ip) {
            return t;
        }
    }
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        struct tcpcb *t = &tcbs[i];
        if (t->in_use && t->state == TCP_LISTEN && t->local_port == local_port) {
            return t;
        }
    }
    return (struct tcpcb *)0;
}

static int port_in_use(uint16_t port) {
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        if (tcbs[i].in_use && tcbs[i].local_port == port) {
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

/* An initial sequence number that is not zero and not the same twice.
 * RFC 793 wants a clock-driven one so a segment from a previous
 * incarnation of the same four-tuple cannot be accepted by this one;
 * a tick-derived counter is what this machine has, and it is a great
 * deal better than the constant it would otherwise be. */
static uint32_t next_isn(void) {
    isn_counter += 64000;
    return isn_counter + tick_count * 250000u;
}

/* ---- the public surface ----------------------------------------------- */

void tcp_init(void) {
    k_memset(tcbs, 0, sizeof(tcbs));
    tick_count = 0;
    isn_counter = 0x1EA50000u;
    ephemeral = TCP_EPHEMERAL_FIRST;
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
    /* The source address a loopback connection uses has to be one the
     * other end will accept as a destination, and net_is_local_ip covers
     * both - so a connection to 127.0.0.1 is sourced from 127.0.0.1 and
     * the four-tuple matches on the way back. */
    t->local_ip = ((ip & NET_LOOPBACK_MASK) == NET_LOOPBACK_NET) ? ip : net_local_ip();
    t->remote_ip = ip;
    t->remote_port = port;
    t->iss = next_isn();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss;
    t->snd_wnd = TCP_DEFAULT_MSS;
    t->state = TCP_SYN_SENT;
    t->connect_deadline = tick_count + 100; /* ten seconds */

    emit(t, t->iss, TCP_SYN, (const uint8_t *)0, 0, 1);
    t->snd_nxt = t->iss + 1;
    arm_rtx(t);
    return 0;
}

struct tcpcb *tcp_accept(struct tcpcb *listener) {
    if (!listener || listener->state != TCP_LISTEN) {
        return (struct tcpcb *)0;
    }
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        struct tcpcb *t = &tcbs[i];
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
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        const struct tcpcb *t = &tcbs[i];
        if (t->in_use && t->listener == listener && t->pending_accept &&
            t->state == TCP_ESTABLISHED) {
            n++;
        }
    }
    return n;
}

int tcp_send(struct tcpcb *t, const uint8_t *data, uint16_t len) {
    if (!t || !t->in_use || t->fin_sent) {
        return -1;
    }
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) {
        return -1;
    }
    uint32_t room = TCP_SEND_BUF - t->send_len;
    uint32_t n = len < room ? len : room;
    if (n) {
        k_memcpy(t->send_buf + t->send_len, data, n);
        t->send_len += n;
        try_send(t);
    }
    return (int)n;
}

int tcp_recv(struct tcpcb *t, uint8_t *out, uint16_t max) {
    if (!t || !t->in_use) {
        return -1;
    }
    if (t->recv_len == 0) {
        /* End of stream is a different answer from "nothing right now",
         * and a reader that cannot tell them apart either spins forever
         * or stops early. */
        if (t->peer_fin || t->reset || t->state == TCP_CLOSED) {
            return -1;
        }
        return 0;
    }
    uint32_t n = t->recv_len < max ? t->recv_len : max;
    k_memcpy(out, t->recv_buf, n);
    t->recv_len -= n;
    if (t->recv_len) {
        k_memmove(t->recv_buf, t->recv_buf + n, t->recv_len);
    }
    /* The window just opened. Telling the peer immediately is the
     * difference between a transfer that streams and one that stalls
     * until the next timer - silly-window avoidance in the one direction
     * where it costs a single segment to get right. */
    if (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2) {
        send_ack(t);
    }
    return (int)n;
}

int tcp_bytes_available(const struct tcpcb *t) {
    return (t && t->in_use) ? (int)t->recv_len : 0;
}

int tcp_send_space(const struct tcpcb *t) {
    return (t && t->in_use) ? (int)(TCP_SEND_BUF - t->send_len) : 0;
}

tcp_state_t tcp_state(const struct tcpcb *t) {
    return (t && t->in_use) ? t->state : TCP_CLOSED;
}

uint16_t tcp_local_port(const struct tcpcb *t) { return t ? t->local_port : 0; }
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
        /* A listener owns every connection that completed on it and was
         * never accepted. Closing it has to take them too, or the
         * control blocks are leaked and - worse - the peers are left
         * holding connections nothing on this machine will ever read.
         * An RST is the truthful answer to both. */
        for (int i = 0; i < TCP_MAX_TCBS; i++) {
            if (tcbs[i].in_use && tcbs[i].listener == t) {
                /* Nothing above holds these - they were never accepted -
                 * so the listener releases them on their behalf. */
                tcbs[i].released = 1;
                tcp_abort(&tcbs[i]);
            }
        }
        tcb_dispose(t);
        return;
    case TCP_CLOSED:
        /* Allocated and never connected - a socket that failed before it
         * got anywhere. There is nothing to say to anybody; give the
         * slot back. Without this case it fell through to the "already
         * closing" default and the control block was never freed, which
         * is a sixteen-connection table leaking one entry per failed
         * accept. */
        tcb_dispose(t);
        return;
    case TCP_SYN_SENT:
        tcb_dispose(t);
        return;
    case TCP_ESTABLISHED:
        t->fin_sent = 1;
        t->fin_seq = t->snd_una + t->send_len;
        t->state = TCP_FIN_WAIT_1;
        try_send(t);
        return;
    case TCP_CLOSE_WAIT:
        t->fin_sent = 1;
        t->fin_seq = t->snd_una + t->send_len;
        t->state = TCP_LAST_ACK;
        try_send(t);
        return;
    default:
        /* Already closing, or closed. A second close is a no-op rather
         * than an error: the fd is gone either way. */
        return;
    }
}

void tcp_abort(struct tcpcb *t) {
    if (!t || !t->in_use) {
        return;
    }
    if (t->state != TCP_LISTEN && t->state != TCP_CLOSED && t->remote_port) {
        emit(t, t->snd_nxt, TCP_RST, (const uint8_t *)0, 0, 0);
    }
    tcb_dispose(t);
}

/* ---- receiving --------------------------------------------------------- */

static void parse_mss(struct tcpcb *t, const uint8_t *seg, uint16_t data_off) {
    uint16_t i = TCP_HEADER_LEN;
    while (i + 1 < data_off) {
        uint8_t kind = seg[i];
        if (kind == 0) { break; }       /* end of option list */
        if (kind == 1) { i++; continue; } /* no-op padding */
        uint8_t olen = seg[i + 1];
        if (olen < 2 || i + olen > data_off) { break; }
        if (kind == 2 && olen == 4) {
            uint16_t mss = read_be16(seg + i + 2);
            if (mss > 0) {
                t->mss = mss < TCP_MAX_MSS ? mss : TCP_MAX_MSS;
                /* RFC 5681's initial window, now that the MSS is known. */
                t->cwnd = 2u * t->mss;
            }
        }
        i = (uint16_t)(i + olen);
    }
}

/* Drops acknowledged bytes off the front of the send buffer and runs the
 * congestion-control response to the ACK. Returns 1 if anything new was
 * acknowledged. */
static int process_ack(struct tcpcb *t, uint32_t ack, uint32_t window) {
    t->snd_wnd = window;

    if (seq_leq(ack, t->snd_una)) {
        /* A duplicate ACK: same acknowledgement, no data, window
         * unchanged. Three of them mean a segment was lost and the ones
         * after it arrived - fast retransmit, RFC 5681 section 3.2, and
         * the reason a single loss costs one round trip instead of a
         * whole retransmission timeout. */
        if (ack == t->snd_una && t->send_len > 0) {
            t->dup_acks++;
            if (t->dup_acks == 3) {
                t->ssthresh = (t->snd_nxt - t->snd_una) / 2;
                if (t->ssthresh < 2u * t->mss) { t->ssthresh = 2u * t->mss; }
                t->recover = t->snd_nxt;
                emit(t, t->snd_una, TCP_ACK | TCP_PSH, t->send_buf, (uint16_t)(t->send_len < t->mss ? t->send_len : t->mss), 0);
                debug_retransmits++;
                t->cwnd = t->ssthresh + 3u * t->mss;
                arm_rtx(t);
            } else if (t->dup_acks > 3) {
                t->cwnd += t->mss; /* inflate for each segment that left the network */
                try_send(t);
            }
        }
        return 0;
    }
    if (seq_gt(ack, t->snd_nxt)) {
        return 0; /* acknowledging something we never sent */
    }

    uint32_t acked = ack - t->snd_una;

    /* The FIN occupies one sequence number and is not in send_buf. */
    uint32_t data_acked = acked;
    if (t->fin_sent && seq_gt(ack, t->fin_seq)) {
        data_acked = acked - 1;
    }
    if (data_acked > t->send_len) {
        data_acked = t->send_len;
    }
    if (data_acked) {
        t->send_len -= data_acked;
        if (t->send_len) {
            k_memmove(t->send_buf, t->send_buf + data_acked, t->send_len);
        }
    }
    t->snd_una = ack;

    if (t->dup_acks >= 3) {
        /* Leaving fast recovery: deflate the window back to ssthresh. */
        t->cwnd = t->ssthresh;
    } else if (t->cwnd < t->ssthresh) {
        t->cwnd += t->mss;                                  /* slow start: one MSS per ACK */
    } else {
        uint32_t inc = (uint32_t)t->mss * t->mss / (t->cwnd ? t->cwnd : 1);
        t->cwnd += inc ? inc : 1;                           /* congestion avoidance: one MSS per RTT */
    }
    t->dup_acks = 0;

    if (t->rtt_timing && seq_geq(ack, t->rtt_seq)) {
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

static void deliver(struct tcpcb *t, const uint8_t *data, uint16_t len) {
    uint32_t room = TCP_RECV_BUF - t->recv_len;
    uint32_t n = len < room ? len : room;
    if (n) {
        k_memcpy(t->recv_buf + t->recv_len, data, n);
        t->recv_len += n;
        t->rcv_nxt += n;
    }
}

void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg, uint16_t len) {
    if (len < TCP_HEADER_LEN) {
        return;
    }
    uint16_t data_off = (uint16_t)((seg[12] >> 4) * 4);
    if (data_off < TCP_HEADER_LEN || data_off > len) {
        return;
    }
    if (fold(sum16(pseudo_sum(src_ip, dst_ip, len), seg, len)) != 0) {
        return; /* a corrupt segment is one that never arrived */
    }

    uint16_t src_port = read_be16(seg + 0);
    uint16_t dst_port = read_be16(seg + 2);
    uint32_t seq = read_be32(seg + 4);
    uint32_t ack = read_be32(seg + 8);
    uint8_t flags = seg[13];
    uint16_t window = read_be16(seg + 14);
    const uint8_t *data = seg + data_off;
    uint16_t data_len = (uint16_t)(len - data_off);

    struct tcpcb *t = find_tcb(dst_ip, dst_port, src_ip, src_port);
    if (!t) {
        send_reset(src_ip, dst_ip, seg, len);
        return;
    }

    if (t->state == TCP_LISTEN) {
        if (!(flags & TCP_SYN) || (flags & TCP_ACK)) {
            send_reset(src_ip, dst_ip, seg, len);
            return;
        }
        struct tcpcb *c = alloc_tcb();
        if (!c) {
            send_reset(src_ip, dst_ip, seg, len); /* the backlog is full; refusing beats hanging */
            return;
        }
        c->local_ip = dst_ip;
        c->local_port = dst_port;
        c->remote_ip = src_ip;
        c->remote_port = src_port;
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
        emit(c, c->iss, TCP_SYN | TCP_ACK, (const uint8_t *)0, 0, 1);
        c->snd_nxt = c->iss + 1;
        arm_rtx(c);
        return;
    }

    if (t->state == TCP_SYN_SENT) {
        if (flags & TCP_RST) {
            /* Refused. This is the answer a connection to a port nothing
             * is listening on gets, and it must be told apart from a
             * connection that is merely slow. */
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
        t->reset = 1;
        if (t->state == TCP_TIME_WAIT || t->state == TCP_LAST_ACK) {
            tcb_dispose(t);
        } else {
            t->state = TCP_CLOSED;
        }
        return;
    }

    /* The acceptance test, RFC 793 3.3. A segment outside the receive
     * window is not data we can use - it is either a retransmission of
     * something already taken or something ahead of a gap. Either way
     * the answer is an ACK saying where we actually are, which is what
     * makes the peer retransmit the right thing.
     *
     * The exception is a segment carrying no sequence space at all: a
     * bare ACK's sequence number is about the *sender's* stream, and it
     * is accepted at *any* seq - not merely anywhere in the window.
     * Go-back-N can legitimately put a peer's snd_nxt behind our rcv_nxt
     * (its retransmission regressed) or beyond our advertised window
     * (its data outran what we have taken), and either way its bare ACK
     * still carries the one thing that resynchronizes the two ends.
     * Windowing it was a livelock: each side judged the other's ACK
     * unacceptable, discarded the acknowledgement it carried, and
     * answered with a challenge ACK the other side judged unacceptable
     * in turn - which on the loopback queue, where every send is
     * delivered synchronously, is an infinite ACK war inside one
     * syscall that wedges the whole machine. */
    int carries_no_sequence = (data_len == 0) && !(flags & (TCP_SYN | TCP_FIN));
    if (seq != t->rcv_nxt && !carries_no_sequence) {
        if (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
            t->state == TCP_FIN_WAIT_2 || t->state == TCP_CLOSE_WAIT) {
            send_ack(t);
        }
        return;
    }

    if (flags & TCP_ACK) {
        process_ack(t, ack, window);
        if (t->state == TCP_SYN_RECEIVED && seq_geq(t->snd_una, t->iss + 1)) {
            t->state = TCP_ESTABLISHED;
        }
    }

    if (data_len && seq == t->rcv_nxt &&
        (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
         t->state == TCP_FIN_WAIT_2)) {
        deliver(t, data, data_len);
        send_ack(t);
    }

    if ((flags & TCP_FIN) && seq == t->rcv_nxt) {
        t->rcv_nxt++;
        t->peer_fin = 1;
        send_ack(t);
        switch (t->state) {
        case TCP_ESTABLISHED:
        case TCP_SYN_RECEIVED:
            t->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            /* Simultaneous close if our own FIN is still unacknowledged;
             * otherwise straight to TIME_WAIT. */
            t->state = (t->fin_sent && seq_gt(t->snd_una, t->fin_seq)) ? TCP_TIME_WAIT : TCP_CLOSING;
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

    /* State transitions driven by our own FIN being acknowledged. */
    if (t->fin_sent && seq_gt(t->snd_una, t->fin_seq)) {
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

/* ---- the clock --------------------------------------------------------- */

void tcp_tick(void) {
    /* M67 boundary 2 of 3: the retransmission clock's own thread. See
     * net.h - without this the 100 ms tick walks every TCB while a
     * syscall on another core is inside one of them. */
    net_lock_acquire();
    tick_count++;

    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        struct tcpcb *t = &tcbs[i];
        if (!t->in_use) {
            continue;
        }

        /* A block the socket layer has let go of, whose state machine
         * has stalled - FIN_WAIT_2 waiting on a peer that stopped
         * answering is the classic one. Sixteen slots cannot be left to
         * neglect. */
        if (t->released && t->linger_deadline && seq_geq(tick_count, t->linger_deadline)) {
            tcb_dispose(t);
            continue;
        }

        if (t->state == TCP_TIME_WAIT) {
            /* 2*MSL is two minutes on a real network and two seconds
             * here, and the difference is worth being honest about: this
             * exists so a delayed segment from this connection cannot be
             * accepted by the next one with the same four-tuple, and
             * two seconds is enough for a machine talking to itself and
             * would not be enough for the Internet. Shortened so a boot
             * self-test can watch the state actually expire rather than
             * assert that a timer was set. */
            if (seq_geq(tick_count, t->time_wait_deadline)) {
                tcb_dispose(t);
            }
            continue;
        }

        if (t->state == TCP_SYN_SENT && seq_geq(tick_count, t->connect_deadline)) {
            t->connect_failed = 1;
            tcb_dispose(t);
            continue;
        }

        if (t->rtx_pending && seq_geq(tick_count, t->rtx_deadline)) {
            /* A timeout means the network is congested, not merely that
             * a segment was unlucky - so ssthresh drops to half the
             * flight size and cwnd goes all the way back to one segment.
             * RFC 5681 section 3.1. */
            t->ssthresh = (t->snd_nxt - t->snd_una) / 2;
            if (t->ssthresh < 2u * t->mss) { t->ssthresh = 2u * t->mss; }
            t->cwnd = t->mss;
            t->dup_acks = 0;

            t->backoff++;
            if (t->backoff > 6) {
                /* Six doublings from a one-second floor is over a minute
                 * of trying. A peer that has not answered in that time
                 * is gone, and a connection that never gives up is a
                 * program that never returns. */
                t->connect_failed = 1;
                t->reset = 1;
                tcb_dispose(t);
                continue;
            }
            t->rto *= 2;
            if (t->rto > 600) { t->rto = 600; }
            t->rtt_timing = 0; /* Karn: do not measure a retransmitted segment */

            debug_retransmits++;

            /* Resend from snd_una - go-back-N, which is what a stack
             * without SACK can do and is why the receiver dropping
             * out-of-order segments costs nothing extra here. */
            if (t->send_len) {
                uint32_t n = t->send_len < t->mss ? t->send_len : t->mss;
                emit(t, t->snd_una, TCP_ACK | TCP_PSH, t->send_buf, (uint16_t)n, 0);
            } else if (t->state == TCP_SYN_SENT) {
                emit(t, t->iss, TCP_SYN, (const uint8_t *)0, 0, 1);
            } else if (t->state == TCP_SYN_RECEIVED) {
                emit(t, t->iss, TCP_SYN | TCP_ACK, (const uint8_t *)0, 0, 1);
            } else if (t->fin_sent) {
                emit(t, t->fin_seq, TCP_FIN | TCP_ACK, (const uint8_t *)0, 0, 0);
            }
            t->rtx_deadline = tick_count + t->rto;
        }
    }
    net_lock_release();
}
