#include "unix_socket.h"

#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

static spinlock_t unix_lock;

typedef struct unixseg {
    uint32_t len;
    int nfds;
    file_descriptor_slot_t fds[UNIX_MAX_FDS];
} unixseg_t;

typedef struct unix_socket {
    int type;
    int refs;
    struct unix_socket *peer;
    int shut_rd, shut_wr;
    int listening;
    int bound;

    uint8_t buf[UNIX_BUF_SIZE];
    uint32_t head;
    uint32_t count;
    unixseg_t segs[UNIX_MAX_SEGS];
    int seg_head, seg_count;

    struct unix_socket *backlog[UNIX_BACKLOG];
    int backlog_count;
} unix_socket_t;

typedef struct {
    int len;
    char name[UNIX_PATH_MAX];
    unix_socket_t *sock;
} unixname_t;

static unixname_t names[UNIX_MAX_NAMES];
static int live_count;
static int queued_file_descriptor_count;

void unix_socket_init(void) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        names[i].len = 0;
        names[i].sock = (unix_socket_t *)0;
    }
    live_count = 0;
    queued_file_descriptor_count = 0;
    spin_unlock_irqrestore(&unix_lock, f);
}

static void unix_wake(void) {
    scheduler_wake_all(SCHED_POLL_CHAN);
}

static unix_socket_t *unix_new(int type) {
    if (type != UNIX_SOCK_STREAM && type != UNIX_SOCK_SEQPACKET) {
        return (unix_socket_t *)0;
    }
    if (live_count >= UNIX_MAX_SOCKETS) {
        return (unix_socket_t *)0;
    }
    unix_socket_t *s = (unix_socket_t *)kmalloc(sizeof(unix_socket_t));
    if (!s) {
        return (unix_socket_t *)0;
    }
    k_memset(s, 0, sizeof(*s));
    s->type = type;
    s->refs = 1;
    live_count++;
    return s;
}

struct unix_socket *unix_socket_alloc(int type) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    unix_socket_t *s = unix_new(type);
    spin_unlock_irqrestore(&unix_lock, f);
    return s;
}

int unix_socket_pair(int type, struct unix_socket **a_out, struct unix_socket **b_out) {
    if (!a_out || !b_out) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    unix_socket_t *a = unix_new(type);
    unix_socket_t *b = unix_new(type);
    if (!a || !b) {
        if (a) { kfree(a); live_count--; }
        if (b) { kfree(b); live_count--; }
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    a->peer = b;
    b->peer = a;
    spin_unlock_irqrestore(&unix_lock, f);
    *a_out = a;
    *b_out = b;
    return 0;
}

int unix_socket_type(const struct unix_socket *s) {
    return s ? s->type : -1;
}

void unix_socket_reference(struct unix_socket *s) {
    if (!s) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    s->refs++;
    spin_unlock_irqrestore(&unix_lock, f);
}

static void unix_release_queued_file_descriptors(unix_socket_t *s) {
    for (int i = 0; i < s->seg_count; i++) {
        unixseg_t *seg = &s->segs[(s->seg_head + i) % UNIX_MAX_SEGS];
        for (int j = 0; j < seg->nfds; j++) {
            file_descriptor_release(&seg->fds[j]);
        }
        seg->nfds = 0;
    }
    s->seg_count = 0;
}

void unix_socket_unref(struct unix_socket *s) {
    if (!s) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (--s->refs > 0) {
        spin_unlock_irqrestore(&unix_lock, f);
        return;
    }
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        if (names[i].sock == s) {
            names[i].len = 0;
            names[i].sock = (unix_socket_t *)0;
        }
    }
    if (s->peer) {
        s->peer->peer = (unix_socket_t *)0;
        s->peer = (unix_socket_t *)0;
    }
    unix_socket_t *pending[UNIX_BACKLOG];
    int npending = s->backlog_count;
    for (int i = 0; i < npending; i++) {
        pending[i] = s->backlog[i];
    }
    s->backlog_count = 0;
    for (int i = 0; i < s->seg_count; i++) {
        queued_file_descriptor_count -= s->segs[(s->seg_head + i) % UNIX_MAX_SEGS].nfds;
    }
    live_count--;
    spin_unlock_irqrestore(&unix_lock, f);

    unix_wake();

    unix_release_queued_file_descriptors(s);
    for (int i = 0; i < npending; i++) {
        unix_socket_unref(pending[i]);
    }
    kfree(s);
}

static int name_eq(const unixname_t *e, const char *name, int len) {
    if (e->len != len) {
        return 0;
    }
    for (int i = 0; i < len; i++) {
        if (e->name[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

static unix_socket_t *name_lookup(const char *name, int len) {
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        if (names[i].len && name_eq(&names[i], name, len)) {
            return names[i].sock;
        }
    }
    return (unix_socket_t *)0;
}

int unix_socket_bind(struct unix_socket *s, const char *name, int len) {
    if (!s || !name || len <= 0 || len > UNIX_PATH_MAX) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->bound || s->peer) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    if (name_lookup(name, len)) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        if (!names[i].len) {
            for (int j = 0; j < len; j++) {
                names[i].name[j] = name[j];
            }
            names[i].len = len;
            names[i].sock = s;
            s->bound = 1;
            spin_unlock_irqrestore(&unix_lock, f);
            return 0;
        }
    }
    spin_unlock_irqrestore(&unix_lock, f);
    return -1;
}

int unix_socket_listen(struct unix_socket *s) {
    if (!s) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int ok = s->bound && !s->peer;
    if (ok) {
        s->listening = 1;
    }
    spin_unlock_irqrestore(&unix_lock, f);
    return ok ? 0 : -1;
}

int unix_socket_connect(struct unix_socket *s, const char *name, int len) {
    if (!s || !name || len <= 0 || len > UNIX_PATH_MAX) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->peer || s->listening) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    unix_socket_t *listener = name_lookup(name, len);
    if (!listener || !listener->listening || listener->type != s->type ||
        listener->backlog_count >= UNIX_BACKLOG) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    unix_socket_t *srv = unix_new(s->type);
    if (!srv) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    srv->peer = s;
    s->peer = srv;
    listener->backlog[listener->backlog_count++] = srv;
    spin_unlock_irqrestore(&unix_lock, f);
    unix_wake();
    return 0;
}

struct unix_socket *unix_socket_accept(struct unix_socket *listener) {
    if (!listener) {
        return (unix_socket_t *)0;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (!listener->listening || listener->backlog_count == 0) {
        spin_unlock_irqrestore(&unix_lock, f);
        return (unix_socket_t *)0;
    }
    unix_socket_t *conn = listener->backlog[0];
    for (int i = 1; i < listener->backlog_count; i++) {
        listener->backlog[i - 1] = listener->backlog[i];
    }
    listener->backlog_count--;
    spin_unlock_irqrestore(&unix_lock, f);
    return conn;
}

static void ring_write(unix_socket_t *d, const uint8_t *src, uint32_t len) {
    uint32_t tail = (d->head + d->count) % UNIX_BUF_SIZE;
    uint32_t first = UNIX_BUF_SIZE - tail;
    if (first > len) {
        first = len;
    }
    k_memcpy(&d->buf[tail], src, first);
    if (len > first) {
        k_memcpy(&d->buf[0], src + first, len - first);
    }
    d->count += len;
}

static void ring_read(unix_socket_t *s, uint8_t *dst, uint32_t len) {
    uint32_t first = UNIX_BUF_SIZE - s->head;
    if (first > len) {
        first = len;
    }
    k_memcpy(dst, &s->buf[s->head], first);
    if (len > first) {
        k_memcpy(dst + first, &s->buf[0], len - first);
    }
    s->head = (s->head + len) % UNIX_BUF_SIZE;
    s->count -= len;
}

static void ring_discard(unix_socket_t *s, uint32_t len) {
    s->head = (s->head + len) % UNIX_BUF_SIZE;
    s->count -= len;
}

long unix_socket_send(struct unix_socket *s, const uint8_t *data, uint32_t len,
                   const file_descriptor_slot_t *fds, int nfds) {
    if (!s || nfds < 0 || nfds > UNIX_MAX_FDS || (nfds > 0 && !fds)) {
        return -1;
    }
    file_descriptor_slot_t kept[UNIX_MAX_FDS];
    for (int i = 0; i < nfds; i++) {
        kept[i] = fds[i];
        file_descriptor_retain(&kept[i]);
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    unix_socket_t *d = s->peer;
    long fail = -1;
    int refuse = 0;
    if (!d || s->shut_wr || d->shut_rd) {
        refuse = 1;
    } else if (s->type == UNIX_SOCK_SEQPACKET && len > UNIX_BUF_SIZE) {
        refuse = 1;
    }
    if (refuse) {
        spin_unlock_irqrestore(&unix_lock, f);
        for (int i = 0; i < nfds; i++) {
            file_descriptor_release(&kept[i]);
        }
        return fail;
    }
    uint32_t space = UNIX_BUF_SIZE - d->count;
    uint32_t take = len > space ? space : len;
    if (s->type == UNIX_SOCK_SEQPACKET && take < len) {
        take = 0;
    }
    unixseg_t *tail = d->seg_count ? &d->segs[(d->seg_head + d->seg_count - 1) % UNIX_MAX_SEGS]
                                   : (unixseg_t *)0;
    int need_seg = (s->type == UNIX_SOCK_SEQPACKET) || nfds > 0 || !tail || tail->nfds > 0;
    int would_block = (need_seg && d->seg_count >= UNIX_MAX_SEGS) || (len > 0 && take == 0);
    if (would_block) {
        spin_unlock_irqrestore(&unix_lock, f);
        for (int i = 0; i < nfds; i++) {
            file_descriptor_release(&kept[i]);
        }
        return 0;
    }
    if (len == 0 && nfds == 0) {
        spin_unlock_irqrestore(&unix_lock, f);
        return 0;
    }
    if (need_seg) {
        unixseg_t *seg = &d->segs[(d->seg_head + d->seg_count) % UNIX_MAX_SEGS];
        seg->len = take;
        seg->nfds = nfds;
        for (int i = 0; i < nfds; i++) {
            seg->fds[i] = kept[i];
        }
        d->seg_count++;
        queued_file_descriptor_count += nfds;
    } else {
        tail->len += take;
    }
    if (take) {
        ring_write(d, data, take);
    }
    spin_unlock_irqrestore(&unix_lock, f);
    unix_wake();
    return (long)take;
}

long unix_socket_receive(struct unix_socket *s, uint8_t *out, uint32_t max,
                   file_descriptor_slot_t *file_descriptors_out, int max_fds, int *nfds_out,
                   int *flags_out) {
    if (nfds_out) {
        *nfds_out = 0;
    }
    if (flags_out) {
        *flags_out = 0;
    }
    if (!s) {
        return -1;
    }
    file_descriptor_slot_t dropped[UNIX_MAX_FDS];
    int ndropped = 0;
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->shut_rd) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    if (s->count == 0 && s->seg_count == 0) {
        int eof = (!s->peer || s->peer->shut_wr);
        spin_unlock_irqrestore(&unix_lock, f);
        return eof ? -1 : 0;
    }
    uint32_t got = 0;
    int took_file_descriptors = 0;
    while (s->seg_count > 0) {
        unixseg_t *seg = &s->segs[s->seg_head];
        if (seg->nfds > 0) {
            if (took_file_descriptors) {
                break;
            }
            int n = seg->nfds;
            int keep = n < max_fds ? n : max_fds;
            for (int i = 0; i < keep; i++) {
                file_descriptors_out[i] = seg->fds[i];
            }
            for (int i = keep; i < n; i++) {
                dropped[ndropped++] = seg->fds[i];
            }
            queued_file_descriptor_count -= n;
            seg->nfds = 0;
            if (nfds_out) {
                *nfds_out = keep;
            }
            if (keep < n && flags_out) {
                *flags_out |= UNIX_RECV_CTRUNC;
            }
            took_file_descriptors = 1;
        }
        uint32_t room = max - got;
        if (room == 0 && seg->len > 0) {
            break;
        }
        uint32_t n = seg->len < room ? seg->len : room;
        if (n) {
            ring_read(s, out + got, n);
            got += n;
            seg->len -= n;
        }
        if (seg->len == 0) {
            s->seg_head = (s->seg_head + 1) % UNIX_MAX_SEGS;
            s->seg_count--;
        } else if (s->type == UNIX_SOCK_SEQPACKET) {
            ring_discard(s, seg->len);
            seg->len = 0;
            s->seg_head = (s->seg_head + 1) % UNIX_MAX_SEGS;
            s->seg_count--;
            if (flags_out) {
                *flags_out |= UNIX_RECV_TRUNC;
            }
        }
        if (s->type == UNIX_SOCK_SEQPACKET) {
            break;
        }
        if (got >= max) {
            break;
        }
        if (s->seg_count > 0 && s->segs[s->seg_head].nfds > 0) {
            break;
        }
    }
    spin_unlock_irqrestore(&unix_lock, f);

    for (int i = 0; i < ndropped; i++) {
        file_descriptor_release(&dropped[i]);
    }
    if (got > 0 || took_file_descriptors) {
        unix_wake();
        return (long)got;
    }
    return 0;
}

int unix_socket_pending(const struct unix_socket *s) {
    if (!s) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int ready;
    if (s->listening) {
        ready = s->backlog_count > 0;
    } else {
        ready = s->count > 0 || s->seg_count > 0 || !s->peer || s->peer->shut_wr ||
                s->shut_rd;
    }
    spin_unlock_irqrestore(&unix_lock, f);
    return ready ? 1 : 0;
}

int unix_socket_writable(const struct unix_socket *s) {
    if (!s) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int w;
    if (!s->peer || s->shut_wr || s->peer->shut_rd) {
        w = 1;
    } else {
        unix_socket_t *d = s->peer;
        unixseg_t *tail = d->seg_count
                              ? &d->segs[(d->seg_head + d->seg_count - 1) % UNIX_MAX_SEGS]
                              : (unixseg_t *)0;
        int has_record = tail && tail->nfds == 0 && s->type != UNIX_SOCK_SEQPACKET;
        w = d->count < UNIX_BUF_SIZE && (has_record || d->seg_count < UNIX_MAX_SEGS);
    }
    spin_unlock_irqrestore(&unix_lock, f);
    return w;
}

int unix_socket_hup(const struct unix_socket *s) {
    if (!s) {
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int hup = (!s->peer || s->peer->shut_wr || s->shut_rd) && s->count == 0 &&
              s->seg_count == 0;
    spin_unlock_irqrestore(&unix_lock, f);
    return hup;
}

int unix_socket_rdhup(const struct unix_socket *s) {
    if (!s) {
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int rd = (!s->peer || s->peer->shut_wr);
    spin_unlock_irqrestore(&unix_lock, f);
    return rd;
}

int unix_socket_shutdown(struct unix_socket *s, int how) {
    if (!s || how < 0 || how > 2) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (how == 0 || how == 2) {
        s->shut_rd = 1;
    }
    if (how == 1 || how == 2) {
        s->shut_wr = 1;
    }
    spin_unlock_irqrestore(&unix_lock, f);
    unix_wake();
    return 0;
}

int unix_socket_in_use(void) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int n = live_count;
    spin_unlock_irqrestore(&unix_lock, f);
    return n;
}

int unix_socket_queued_file_descriptors(void) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int n = queued_file_descriptor_count;
    spin_unlock_irqrestore(&unix_lock, f);
    return n;
}
