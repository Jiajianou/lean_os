#include "unix_socket.h"

#include "drivers/kernel_log.h"

#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

static spinlock_t unix_lock;

typedef struct unixseg {
    uint32_t length;
    int nfds;
    file_descriptor_slot_t file_descriptors[UNIX_MAX_FILE_DESCRIPTORS];
} unixseg_t;

typedef struct unix_socket {
    int type;
    int refs;
    struct unix_socket *peer;
    int shut_rd, shut_wr;
    int listening;
    int bound;

    uint8_t buffer[UNIX_BUFFER_SIZE];
    uint32_t head;
    uint32_t count;
    unixseg_t segs[UNIX_MAX_SEGS];
    int seg_head, seg_count;

    struct unix_socket *backlog[UNIX_BACKLOG];
    int backlog_count;

    /* The process that this end belongs to, for SO_PEERCRED - which reports
       the credentials of the socket at the OTHER end. A connected pair is
       made by the connecting task, so the server's end is stamped with the
       listener's owner rather than with its maker's. */
    int owner_pid;
} unix_socket_t;

typedef struct {
    int length;
    char name[UNIX_PATH_MAX];
    unix_socket_t *sock;
} unixname_t;

static unixname_t names[UNIX_MAX_NAMES];
static int live_count;
static int queued_file_descriptor_count;

void unix_socket_init(void) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        names[i].length = 0;
        names[i].sock = (unix_socket_t *)0;
    }
    live_count = 0;
    queued_file_descriptor_count = 0;
    spin_unlock_irqrestore(&unix_lock, f);
}

static int unix_refusals;

/* M197: a change wakes the pollers of the two sockets it can make ready -
   the one whose buffer changed and the one on the other end, whose room to
   write or whose peer's existence just changed - rather than every poller on
   the machine. */
static void unix_wake(const unix_socket_t *first, const unix_socket_t *second) {
    scheduler_wake_objects(first, second);
}

static unix_socket_t *unix_new(int type) {
    if (type != UNIX_SOCKET_STREAM && type != UNIX_SOCKET_SEQPACKET) {
        return (unix_socket_t *)0;
    }
    if (live_count >= UNIX_MAX_SOCKETS) {
        unix_refusals++;
        if (unix_refusals == 1 || unix_refusals % 64 == 0) {
            kernel_log_puts("[unix] the machine is at its ceiling of ");
            kernel_log_put_dec(UNIX_MAX_SOCKETS);
            kernel_log_puts(" sockets (");
            kernel_log_put_dec((uint32_t)unix_refusals);
            kernel_log_puts(" refusal(s) since boot)\n");
        }
        return (unix_socket_t *)0;
    }
    unix_socket_t *s = (unix_socket_t *)kmalloc(sizeof(unix_socket_t));
    if (!s) {
        return (unix_socket_t *)0;
    }
    k_memset(s, 0, sizeof(*s));
    s->type = type;
    s->refs = 1;
    task_t *self = scheduler_current();
    s->owner_pid = self ? self->tgid : 0;
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

int unix_socket_peer_pid(const struct unix_socket *s) {
    if (!s || !s->peer) {
        return -1;
    }
    return s->peer->owner_pid;
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
            file_descriptor_release(&seg->file_descriptors[j]);
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
            names[i].length = 0;
            names[i].sock = (unix_socket_t *)0;
        }
    }
    unix_socket_t *old_peer = s->peer;
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

    unix_wake(old_peer, s);

    unix_release_queued_file_descriptors(s);
    for (int i = 0; i < npending; i++) {
        unix_socket_unref(pending[i]);
    }
    kfree(s);
}

static int name_eq(const unixname_t *e, const char *name, int length) {
    if (e->length != length) {
        return 0;
    }
    for (int i = 0; i < length; i++) {
        if (e->name[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

static unix_socket_t *name_lookup(const char *name, int length) {
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        if (names[i].length && name_eq(&names[i], name, length)) {
            return names[i].sock;
        }
    }
    return (unix_socket_t *)0;
}

int unix_socket_bind(struct unix_socket *s, const char *name, int length) {
    if (!s || !name || length <= 0 || length > UNIX_PATH_MAX) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->bound || s->peer) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    if (name_lookup(name, length)) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        if (!names[i].length) {
            for (int j = 0; j < length; j++) {
                names[i].name[j] = name[j];
            }
            names[i].length = length;
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

int unix_socket_connect(struct unix_socket *s, const char *name, int length) {
    if (!s || !name || length <= 0 || length > UNIX_PATH_MAX) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->peer || s->listening) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    unix_socket_t *listener = name_lookup(name, length);
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
    srv->owner_pid = listener->owner_pid;
    srv->peer = s;
    s->peer = srv;
    listener->backlog[listener->backlog_count++] = srv;
    spin_unlock_irqrestore(&unix_lock, f);
    unix_wake(listener, s);
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

static void ring_write(unix_socket_t *d, const uint8_t *source, uint32_t length) {
    uint32_t tail = (d->head + d->count) % UNIX_BUFFER_SIZE;
    uint32_t first = UNIX_BUFFER_SIZE - tail;
    if (first > length) {
        first = length;
    }
    k_memcpy(&d->buffer[tail], source, first);
    if (length > first) {
        k_memcpy(&d->buffer[0], source + first, length - first);
    }
    d->count += length;
}

static void ring_read(unix_socket_t *s, uint8_t *destination, uint32_t length) {
    uint32_t first = UNIX_BUFFER_SIZE - s->head;
    if (first > length) {
        first = length;
    }
    k_memcpy(destination, &s->buffer[s->head], first);
    if (length > first) {
        k_memcpy(destination + first, &s->buffer[0], length - first);
    }
    s->head = (s->head + length) % UNIX_BUFFER_SIZE;
    s->count -= length;
}

static void ring_discard(unix_socket_t *s, uint32_t length) {
    s->head = (s->head + length) % UNIX_BUFFER_SIZE;
    s->count -= length;
}

/* M225: SCM_RIGHTS. The descriptors are the CALLER's held copies - each
   with a reference the caller took under its table's lock and keeps until
   this returns (file_descriptor_hold) - so the reference taken here for the
   queue is taken on an object somebody is known to hold. A copy read straight
   out of a slot and retained here afterwards was a reference taken on an
   object a sibling thread's close could already have let go of.

   A slot that names nothing - free, or RESERVED by a thread still filling it
   in - is refused rather than queued: it used to arrive in the receiver as a
   reserved slot nobody would ever fill. */
long unix_socket_send(struct unix_socket *s, const uint8_t *data, uint32_t length,
                   const file_descriptor_slot_t *file_descriptors, int nfds) {
    if (!s || nfds < 0 || nfds > UNIX_MAX_FILE_DESCRIPTORS || (nfds > 0 && !file_descriptors)) {
        return -1;
    }
    for (int i = 0; i < nfds; i++) {
        if (file_descriptors[i].type == FILE_DESCRIPTOR_NONE ||
            file_descriptors[i].type == FILE_DESCRIPTOR_RESERVED) {
            return -1;
        }
    }
    file_descriptor_slot_t kept[UNIX_MAX_FILE_DESCRIPTORS];
    for (int i = 0; i < nfds; i++) {
        kept[i] = file_descriptors[i];
        file_descriptor_retain(&kept[i]);
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    unix_socket_t *d = s->peer;
    long fail = -1;
    int refuse = 0;
    if (!d || s->shut_wr || d->shut_rd) {
        refuse = 1;
    } else if (s->type == UNIX_SOCKET_SEQPACKET && length > UNIX_BUFFER_SIZE) {
        refuse = 1;
    }
    if (refuse) {
        spin_unlock_irqrestore(&unix_lock, f);
        for (int i = 0; i < nfds; i++) {
            file_descriptor_release(&kept[i]);
        }
        return fail;
    }
    uint32_t space = UNIX_BUFFER_SIZE - d->count;
    uint32_t take = length > space ? space : length;
    if (s->type == UNIX_SOCKET_SEQPACKET && take < length) {
        take = 0;
    }
    unixseg_t *tail = d->seg_count ? &d->segs[(d->seg_head + d->seg_count - 1) % UNIX_MAX_SEGS]
                                   : (unixseg_t *)0;
    int need_seg = (s->type == UNIX_SOCKET_SEQPACKET) || nfds > 0 || !tail || tail->nfds > 0;
    int would_block = (need_seg && d->seg_count >= UNIX_MAX_SEGS) || (length > 0 && take == 0);
    if (would_block) {
        spin_unlock_irqrestore(&unix_lock, f);
        for (int i = 0; i < nfds; i++) {
            file_descriptor_release(&kept[i]);
        }
        return 0;
    }
    if (length == 0 && nfds == 0) {
        spin_unlock_irqrestore(&unix_lock, f);
        return 0;
    }
    if (need_seg) {
        unixseg_t *seg = &d->segs[(d->seg_head + d->seg_count) % UNIX_MAX_SEGS];
        seg->length = take;
        seg->nfds = nfds;
        for (int i = 0; i < nfds; i++) {
            seg->file_descriptors[i] = kept[i];
        }
        d->seg_count++;
        queued_file_descriptor_count += nfds;
    } else {
        tail->length += take;
    }
    if (take) {
        ring_write(d, data, take);
    }
    spin_unlock_irqrestore(&unix_lock, f);
    unix_wake(d, s);
    return (long)take;
}

long unix_socket_receive(struct unix_socket *s, uint8_t *out, uint32_t max,
                   file_descriptor_slot_t *file_descriptors_out, int max_file_descriptors, int *nfds_out,
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
    file_descriptor_slot_t dropped[UNIX_MAX_FILE_DESCRIPTORS];
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
            int keep = n < max_file_descriptors ? n : max_file_descriptors;
            for (int i = 0; i < keep; i++) {
                file_descriptors_out[i] = seg->file_descriptors[i];
            }
            for (int i = keep; i < n; i++) {
                dropped[ndropped++] = seg->file_descriptors[i];
            }
            queued_file_descriptor_count -= n;
            seg->nfds = 0;
            if (nfds_out) {
                *nfds_out = keep;
            }
            if (keep < n && flags_out) {
                *flags_out |= UNIX_RECEIVE_CTRUNC;
            }
            took_file_descriptors = 1;
        }
        uint32_t room = max - got;
        if (room == 0 && seg->length > 0) {
            break;
        }
        uint32_t n = seg->length < room ? seg->length : room;
        if (n) {
            ring_read(s, out + got, n);
            got += n;
            seg->length -= n;
        }
        if (seg->length == 0) {
            s->seg_head = (s->seg_head + 1) % UNIX_MAX_SEGS;
            s->seg_count--;
        } else if (s->type == UNIX_SOCKET_SEQPACKET) {
            ring_discard(s, seg->length);
            seg->length = 0;
            s->seg_head = (s->seg_head + 1) % UNIX_MAX_SEGS;
            s->seg_count--;
            if (flags_out) {
                *flags_out |= UNIX_RECEIVE_TRUNC;
            }
        }
        if (s->type == UNIX_SOCKET_SEQPACKET) {
            break;
        }
        if (got >= max) {
            break;
        }
        if (s->seg_count > 0 && s->segs[s->seg_head].nfds > 0) {
            break;
        }
    }
    const unix_socket_t *writer = s->peer;
    spin_unlock_irqrestore(&unix_lock, f);

    for (int i = 0; i < ndropped; i++) {
        file_descriptor_release(&dropped[i]);
    }
    if (got > 0 || took_file_descriptors) {
        unix_wake(s, writer);
        return (long)got;
    }
    return 0;
}

/* How many bytes a read would return right now, which is a count rather
   than the readiness bit unix_socket_pending answers with - FIONREAD asks
   for the number and nothing else in this kernel needed it before. */
int unix_socket_readable_bytes(const struct unix_socket *s) {
    if (!s) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int bytes = (int)s->count;
    spin_unlock_irqrestore(&unix_lock, f);
    return bytes;
}

long unix_socket_peek(struct unix_socket *s, uint8_t *out, uint32_t max) {
    if (!s) {
        return -1;
    }
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
    /* A datagram is the unit on a SEQPACKET socket, so a peek shows the
       message at the head and not the one behind it. */
    uint32_t available = s->count;
    if (s->type == UNIX_SOCKET_SEQPACKET && s->seg_count > 0) {
        available = s->segs[s->seg_head].length;
    }
    uint32_t n = available < max ? available : max;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s->buffer[(s->head + i) % UNIX_BUFFER_SIZE];
    }
    spin_unlock_irqrestore(&unix_lock, f);
    return (long)n;
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
    if (s->listening) {
        /* Nothing can be written to a listener; Linux never reports one
           writable. */
        w = 0;
    } else if (!s->peer || s->shut_wr || s->peer->shut_rd) {
        w = 1;
    } else {
        unix_socket_t *d = s->peer;
        unixseg_t *tail = d->seg_count
                              ? &d->segs[(d->seg_head + d->seg_count - 1) % UNIX_MAX_SEGS]
                              : (unixseg_t *)0;
        int has_record = tail && tail->nfds == 0 && s->type != UNIX_SOCKET_SEQPACKET;
        w = d->count < UNIX_BUFFER_SIZE && (has_record || d->seg_count < UNIX_MAX_SEGS);
    }
    spin_unlock_irqrestore(&unix_lock, f);
    return w;
}

int unix_socket_hup(const struct unix_socket *s) {
    if (!s) {
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    /* A LISTENING socket has no peer and never will, and that is not a
       hang-up: on Linux a listener reports nothing until a connection is
       queued, and then only that it is readable. Reporting HUP made every
       listener look ready forever, and Chromium's ProcessSingleton answers
       "ready" on its listener with an accept() that is blocking - it makes
       the socket non-blocking inside a DCHECK, which a release build does not
       evaluate - so the browser's IO thread sat in accept() with nothing to
       accept, and no IO task ran in the browser again (M187). */
    int hup = !s->listening &&
              (!s->peer || s->peer->shut_wr || s->shut_rd) && s->count == 0 &&
              s->seg_count == 0;
    spin_unlock_irqrestore(&unix_lock, f);
    return hup;
}

int unix_socket_rdhup(const struct unix_socket *s) {
    if (!s) {
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int rd = !s->listening && (!s->peer || s->peer->shut_wr);
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
    const unix_socket_t *peer = s->peer;
    spin_unlock_irqrestore(&unix_lock, f);
    unix_wake(s, peer);
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
