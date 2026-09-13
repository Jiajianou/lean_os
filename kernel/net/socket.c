#include "socket.h"

#include "lib/libk.h"
#include "net.h"
#include "sched/sched.h"

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t data[SOCKET_MAX_DATAGRAM];
} datagram_t;

struct socket {
    int in_use;
    int refs;
    int type;
    struct tcpcb *tcb;
    int bound;
    uint16_t port;
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint16_t count;
    datagram_t queue[SOCKET_QUEUE_DEPTH];
};

static struct socket sockets[MAX_SOCKETS];

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
    if (handler) {
        raw_handler = handler;
        raw_port = port;
    } else {
        raw_port = 0;
        raw_handler = (socket_raw_handler_t)0;
    }
}

struct socket *socket_alloc(int type) {
    net_lock_acquire();
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
                    net_lock_release();
                    return (struct socket *)0;
                }
            }
            net_lock_release();
            return &sockets[i];
        }
    }
    net_lock_release();
    return (struct socket *)0;
}

struct tcpcb *socket_tcb(struct socket *s) {
    return (s && s->in_use && s->type == SOCK_STREAM) ? s->tcb : (struct tcpcb *)0;
}

void socket_ref(struct socket *s) {
    if (!s) {
        return;
    }
    net_lock_acquire();
    s->refs++;
    net_lock_release();
}

void socket_unref(struct socket *s) {
    if (!s) {
        return;
    }
    net_lock_acquire();
    if (!s->in_use) {
        net_lock_release();
        return;
    }
    if (--s->refs <= 0) {
        if (s->type == SOCK_STREAM && s->tcb) {
            tcp_close(s->tcb);
            tcp_release(s->tcb);
            s->tcb = (struct tcpcb *)0;
        }
        s->bound = 0;
        s->port = 0;
        s->in_use = 0;
    }
    net_lock_release();
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
    if (s->type == SOCK_STREAM) {
        if (tcp_state(s->tcb) == TCP_LISTEN) {
            return tcp_accept_pending(s->tcb);
        }
        return tcp_bytes_available(s->tcb);
    }
    return (int)s->count;
}

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
            return;
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
    sched_wake_all(SCHED_POLL_CHAN);

}
