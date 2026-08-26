#include "socket.h"

#include "lib/libk.h"
#include "net.h"

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t data[SOCKET_MAX_DATAGRAM];
} datagram_t;

struct socket {
    int in_use;
    int refs;
    int type;          /* M66: SOCK_DGRAM or SOCK_STREAM */
    struct tcpcb *tcb; /* M66: NULL for a datagram socket */
    int bound;
    uint16_t port;
    /* A ring, written by the IRQ handler and read by a syscall. head and
     * tail are the only shared mutable state and each is written by
     * exactly one side, which is what makes this safe without a lock on
     * a stack whose RX all happens on one CPU's IRQ. */
    volatile uint16_t head; /* next slot to write */
    volatile uint16_t tail; /* next slot to read */
    volatile uint16_t count;
    datagram_t queue[SOCKET_QUEUE_DEPTH];
};

static struct socket sockets[MAX_SOCKETS];

/* Ephemeral ports from the IANA dynamic range. A rotating cursor rather
 * than a random pick: this kernel has no entropy source worth the name,
 * and rotation gives the one property that actually matters here - a
 * closed socket's port is not immediately handed to the next one, so a
 * late reply to the old socket does not land in the new one's queue. */
#define EPHEMERAL_FIRST 49152u
#define EPHEMERAL_LAST  65535u
static uint32_t ephemeral_cursor = EPHEMERAL_FIRST;

static uint16_t raw_port;
static socket_raw_handler_t raw_handler;

void socket_init(void) {
    k_memset(sockets, 0, sizeof(sockets));
    ephemeral_cursor = EPHEMERAL_FIRST;
    raw_port = 0;
    raw_handler = (socket_raw_handler_t)0;
}

void socket_set_raw_handler(uint16_t port, socket_raw_handler_t handler) {
    /* Handler first, then port: the IRQ side reads the port to decide
     * whether to call, so a port that is live before its handler is
     * installed is a null call. Clearing goes the other way for the same
     * reason - see the store order in socket_unref. */
    if (handler) {
        raw_handler = handler;
        raw_port = port;
    } else {
        raw_port = 0;
        raw_handler = (socket_raw_handler_t)0;
    }
}

struct socket *socket_alloc(int type) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            k_memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].in_use = 1;
            sockets[i].refs = 1;
            sockets[i].type = type;
            if (type == SOCK_STREAM) {
                sockets[i].tcb = tcp_open();
                if (!sockets[i].tcb) {
                    sockets[i].in_use = 0;
                    return (struct socket *)0;
                }
            }
            return &sockets[i];
        }
    }
    return (struct socket *)0;
}

int socket_type(const struct socket *s) {
    return s ? s->type : SOCK_DGRAM;
}

struct tcpcb *socket_tcb(struct socket *s) {
    return (s && s->in_use && s->type == SOCK_STREAM) ? s->tcb : (struct tcpcb *)0;
}

void socket_ref(struct socket *s) {
    if (s) {
        s->refs++;
    }
}

void socket_unref(struct socket *s) {
    if (!s || !s->in_use) {
        return;
    }
    if (--s->refs <= 0) {
        /* M66: a stream socket losing its last descriptor is an active
         * close, not an abort - the peer is entitled to the FIN and to
         * whatever is still in the send buffer. tcp.c owns the control
         * block from here: it runs the state machine down through
         * FIN_WAIT and TIME_WAIT and frees itself, which is why this
         * does not free it. */
        if (s->type == SOCK_STREAM && s->tcb) {
            tcp_close(s->tcb);   /* start the shutdown the peer is owed */
            tcp_release(s->tcb); /* ...and let tcp.c reclaim the slot whenever it is finished */
            s->tcb = (struct tcpcb *)0;
        }
        /* Clear the binding before the slot, so a datagram arriving in
         * an IRQ between these two stores cannot match a port on a
         * socket that is being torn down. */
        s->bound = 0;
        s->port = 0;
        s->in_use = 0;
    }
}

static int port_taken(uint16_t port) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].bound && sockets[i].port == port) {
            return 1;
        }
    }
    return 0;
}

static int bind_ephemeral(struct socket *s) {
    for (uint32_t tried = 0; tried <= EPHEMERAL_LAST - EPHEMERAL_FIRST; tried++) {
        uint16_t port = (uint16_t)ephemeral_cursor;
        ephemeral_cursor = ephemeral_cursor >= EPHEMERAL_LAST ? EPHEMERAL_FIRST : ephemeral_cursor + 1;
        if (!port_taken(port)) {
            s->port = port;
            s->bound = 1;
            return port;
        }
    }
    return -1;
}

int socket_bind(struct socket *s, uint16_t port) {
    if (!s || !s->in_use || s->bound) {
        return -1;
    }
    /* M66: TCP keeps its own port space, because a TCP port and a UDP
     * port with the same number are different ports - conflating them
     * would make binding a UDP socket to 80 refuse an HTTP server. */
    if (s->type == SOCK_STREAM) {
        int p = tcp_bind(s->tcb, port);
        if (p < 0) {
            return -1;
        }
        s->bound = 1;
        s->port = (uint16_t)p;
        return p;
    }
    if (port == 0) {
        return bind_ephemeral(s);
    }
    if (port_taken(port)) {
        return -1;
    }
    s->port = port;
    s->bound = 1;
    return port;
}

int socket_sendto(struct socket *s, uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, uint16_t len) {
    if (!s || !s->in_use || s->type != SOCK_DGRAM || len > UDP_MAX_PAYLOAD) {
        return -1;
    }
    if (!s->bound && bind_ephemeral(s) < 0) {
        return -1;
    }
    if (udp_send(dst_ip, dst_port, s->port, data, len) < 0) {
        return -1;
    }
    return len;
}

int socket_recvfrom(struct socket *s, uint8_t *out, uint16_t max,
                    uint32_t *src_ip_out, uint16_t *src_port_out) {
    if (!s || !s->in_use || s->count == 0) {
        return -1;
    }
    datagram_t *d = &s->queue[s->tail];
    uint16_t n = d->len < max ? d->len : max;
    k_memcpy(out, d->data, n);
    if (src_ip_out) {
        *src_ip_out = d->src_ip;
    }
    if (src_port_out) {
        *src_port_out = d->src_port;
    }
    s->tail = (uint16_t)((s->tail + 1) % SOCKET_QUEUE_DEPTH);
    s->count--;
    return n;
}

int socket_pending(const struct socket *s) {
    if (!s || !s->in_use) {
        return 0;
    }
    /* For a stream this is bytes, for a datagram socket it is
     * datagrams - which is not a inconsistency but the only thing each
     * one can honestly count. A poll on a listening socket answers
     * whether an accept would succeed, which is the third meaning and
     * the only useful one there. */
    if (s->type == SOCK_STREAM) {
        if (tcp_state(s->tcb) == TCP_LISTEN) {
            return tcp_accept_pending(s->tcb);
        }
        return tcp_bytes_available(s->tcb);
    }
    return (int)s->count;
}

/* ---- M66: streams ------------------------------------------------------ */

int socket_listen(struct socket *s) {
    struct tcpcb *tcb = socket_tcb(s);
    if (!tcb) {
        return -1;
    }
    if (!s->bound && socket_bind(s, 0) < 0) {
        return -1;
    }
    return tcp_listen(tcb);
}

int socket_connect(struct socket *s, uint32_t ip, uint16_t port) {
    struct tcpcb *tcb = socket_tcb(s);
    if (!tcb) {
        return -1;
    }
    if (!s->bound && socket_bind(s, 0) < 0) {
        return -1;
    }
    return tcp_connect(tcb, ip, port);
}

struct socket *socket_accept(struct socket *s) {
    struct tcpcb *listener = socket_tcb(s);
    if (!listener) {
        return (struct socket *)0;
    }
    struct tcpcb *conn = tcp_accept(listener);
    if (!conn) {
        return (struct socket *)0;
    }
    /* A socket for the connection tcp.c already built. If the table is
     * full the connection is aborted rather than leaked - the peer gets
     * an RST, which is the truthful answer to "this machine cannot take
     * your connection". */
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            k_memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].in_use = 1;
            sockets[i].refs = 1;
            sockets[i].type = SOCK_STREAM;
            sockets[i].tcb = conn;
            sockets[i].bound = 1;
            sockets[i].port = tcp_local_port(conn);
            return &sockets[i];
        }
    }
    tcp_abort(conn);
    return (struct socket *)0;
}

void socket_deliver(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                    const uint8_t *data, uint16_t len) {
    if (raw_port && dst_port == raw_port && raw_handler) {
        raw_handler(src_ip, src_port, data, len);
        return;
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        struct socket *s = &sockets[i];
        if (!s->in_use || !s->bound || s->port != dst_port) {
            continue;
        }
        if (s->count >= SOCKET_QUEUE_DEPTH) {
            return; /* drop the newest - see socket.h */
        }
        datagram_t *d = &s->queue[s->head];
        d->src_ip = src_ip;
        d->src_port = src_port;
        d->len = len < SOCKET_MAX_DATAGRAM ? len : SOCKET_MAX_DATAGRAM;
        k_memcpy(d->data, data, d->len);
        s->head = (uint16_t)((s->head + 1) % SOCKET_QUEUE_DEPTH);
        s->count++;
        return;
    }
}
