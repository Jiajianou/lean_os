#include "socket.h"

#include "library/kernel_library.h"
#include "memory_management/heap.h"
#include "network.h"
#include "scheduler/scheduler.h"

typedef struct {
    uint32_t source_ip;
    uint16_t source_port;
    uint16_t length;
    uint8_t data[SOCKET_MAX_DATAGRAM];
} datagram_t;

struct socket {
    int in_use;
    int refs;
    int type;
    struct tcpcb *tcb;
    int read_shut;
    int reuse_address;
    int bound;
    uint16_t port;
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint16_t count;
    datagram_t queue[SOCKET_QUEUE_DEPTH];
};

static struct socket *sockets[MAX_SOCKETS];
static int socket_count;

#define EPHEMERAL_FIRST 49152u
#define EPHEMERAL_LAST  65535u
static uint32_t ephemeral_cursor = EPHEMERAL_FIRST;

static uint16_t raw_port;
static socket_raw_handler_t raw_handler;

void socket_init(void) {
    socket_count = 0;
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

static struct socket *claim_socket(int type) {
    net_lock_acquire();
    struct socket *s = (struct socket *)0;
    for (int i = 0; i < socket_count && !s; i++) {
        if (!sockets[i]->in_use) {
            s = sockets[i];
        }
    }
    if (!s && socket_count < MAX_SOCKETS) {
        s = (struct socket *)kmalloc(sizeof(struct socket));
        if (s) {
            sockets[socket_count++] = s;
        }
    }
    if (s) {
        k_memset(s, 0, sizeof(*s));
        s->in_use = 1;
        s->refs = 1;
        s->type = type;
    }
    net_lock_release();
    return s;
}

struct socket *socket_alloc(int type) {
    net_lock_acquire();
    struct socket *s = claim_socket(type);
    if (s && type == SOCK_STREAM) {
        s->tcb = tcp_open();
        if (!s->tcb) {
            s->in_use = 0;
            s = (struct socket *)0;
        }
    }
    net_lock_release();
    return s;
}

struct tcpcb *socket_tcb(struct socket *s) {
    return (s && s->in_use && s->type == SOCK_STREAM) ? s->tcb : (struct tcpcb *)0;
}

void socket_reference(struct socket *s) {
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
    for (int i = 0; i < socket_count; i++) {
        if (sockets[i]->in_use && sockets[i]->bound && sockets[i]->port == port) {
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
        int p = s->reuse_address ? tcp_bind_reusing(s->tcb, port) : tcp_bind(s->tcb, port);
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

/* M226. The one option kept so far. A datagram socket keeps it too, as
   Linux does, and its bind does not consult it - UDP ports here are never
   held past their socket. */
int socket_set_reuse_address(struct socket *s, int on) {
    if (!s || !s->in_use) {
        return -1;
    }
    s->reuse_address = on ? 1 : 0;
    return 0;
}

int socket_reuse_address(const struct socket *s) {
    return (s && s->in_use) ? s->reuse_address : -1;
}

uint16_t socket_local_port(const struct socket *s) {
    if (!s || !s->in_use) {
        return 0;
    }
    if (s->type == SOCK_STREAM && s->tcb) {
        return tcp_local_port(s->tcb);
    }
    return s->port;
}

uint32_t socket_local_ip(const struct socket *s) {
    if (s && s->in_use && s->type == SOCK_STREAM && s->tcb) {
        uint32_t ip = tcp_local_ip(s->tcb);
        if (ip) {
            return ip;
        }
    }
    return net_local_ip();
}

int socket_sendto(struct socket *s, uint32_t destination_ip, uint16_t destination_port,
                  const uint8_t *data, uint16_t length) {
    if (!s || !s->in_use || s->type != SOCK_DGRAM || length > UDP_MAX_PAYLOAD) {
        return -1;
    }
    if (!s->bound && bind_ephemeral(s) < 0) {
        return -1;
    }
    if (udp_send(destination_ip, destination_port, s->port, data, length) < 0) {
        return -1;
    }
    return length;
}

int socket_recvfrom(struct socket *s, uint8_t *out, uint16_t max,
                    uint32_t *source_ip_out, uint16_t *source_port_out) {
    if (!s || !s->in_use || s->count == 0) {
        return -1;
    }
    datagram_t *d = &s->queue[s->tail];
    uint16_t n = d->length < max ? d->length : max;
    k_memcpy(out, d->data, n);
    if (source_ip_out) {
        *source_ip_out = d->source_ip;
    }
    if (source_port_out) {
        *source_port_out = d->source_port;
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

int socket_read_ended(const struct socket *s) {
    if (!s || !s->in_use || s->type != SOCK_STREAM) {
        return 0;
    }
    if (s->read_shut) {
        return 1;
    }
    if (tcp_state(s->tcb) == TCP_LISTEN) {
        return 0;
    }
    return tcp_receive_ended(s->tcb);
}

int socket_read_shut(const struct socket *s) {
    return s && s->in_use && s->read_shut;
}

int socket_shutdown(struct socket *s, int how) {
    if (!s || !s->in_use || s->type != SOCK_STREAM) {
        return -1;
    }
    net_lock_acquire();
    tcp_state_t state = tcp_state(s->tcb);
    if (state == TCP_LISTEN || state == TCP_SYN_SENT ||
        (state == TCP_CLOSED && tcp_remote_port(s->tcb) == 0)) {
        net_lock_release();
        return -1;
    }
    if (how == 0 || how == 2) {
        s->read_shut = 1;
    }
    if ((how == 1 || how == 2) && (state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT)) {
        tcp_close(s->tcb);
    }
    net_lock_release();
    return 0;
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
    struct socket *accepted = claim_socket(SOCK_STREAM);
    if (accepted) {
        accepted->tcb = conn;
        accepted->bound = 1;
        accepted->port = tcp_local_port(conn);
        return accepted;
    }
    tcp_abort(conn);
    return (struct socket *)0;
}

void socket_deliver(uint16_t destination_port, uint32_t source_ip, uint16_t source_port,
                    const uint8_t *data, uint16_t length) {
    if (raw_port && destination_port == raw_port && raw_handler) {
        raw_handler(source_ip, source_port, data, length);
        return;
    }
    for (int i = 0; i < socket_count; i++) {
        struct socket *s = sockets[i];
        if (!s->in_use || !s->bound || s->port != destination_port) {
            continue;
        }
        if (s->count >= SOCKET_QUEUE_DEPTH) {
            return;
        }
        datagram_t *d = &s->queue[s->head];
        d->source_ip = source_ip;
        d->source_port = source_port;
        d->length = length < SOCKET_MAX_DATAGRAM ? length : SOCKET_MAX_DATAGRAM;
        k_memcpy(d->data, data, d->length);
        s->head = (uint16_t)((s->head + 1) % SOCKET_QUEUE_DEPTH);
        s->count++;
        return;
    }
    scheduler_wake_all(SCHEDULER_POLL_CHAN);

}
