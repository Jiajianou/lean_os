/* kernel/ipc/unixsock.c - M118. See unixsock.h for why this exists and
 * why it is in kernel/ipc rather than kernel/net. */
#include "unixsock.h"

#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "sched/sched.h"

/* ---- unix_lock -------------------------------------------------------
 *
 * What it protects: every socket's receive ring and record queue, its
 * refcount, its peer pointer and its flags, plus the name table at the
 * bottom of this file. One lock for all of them, for pipe.c's reason
 * (kernel/ipc/pipe.c's own note): the contention here is two processes
 * taking turns, a lock inside the object would have to be initialised by
 * every path that makes one, and the critical sections are a memcpy long.
 *
 * THE RULE FOR THIS FILE: nothing in here parks, so the lock is never
 * held across a context switch - see the header. It is also never held
 * across fd_release, which reaches into pipe.c and kernel/fs and would
 * make this lock an outer lock over two others for no reason. The two
 * places that have to release descriptors do it after unlocking, on an
 * object no one else can still reach, and each says so.
 *
 * Interrupts off while held: a task dying unrefs its sockets, and that
 * path is reachable from a timer tick delivering SIGKILL. */
static spinlock_t unix_lock;

typedef struct unixseg {
    uint32_t len;               /* bytes of this record still in the ring */
    int nfds;
    fd_slot_t fds[UNIX_MAX_FDS]; /* retained references, handed to whoever reads this record */
} unixseg_t;

typedef struct unixsock {
    int type;
    int refs;
    struct unixsock *peer;      /* NULL once the other end is gone: EOF after the buffer drains */
    int shut_rd, shut_wr;
    int listening;
    int bound;

    /* The RECEIVE side. A socket owns the buffer it is read from and the
     * peer writes into it, which is the arrangement that makes a
     * half-closed direction a flag on one object instead of a shared
     * third one. */
    uint8_t buf[UNIX_BUF_SIZE];
    uint32_t head;              /* next byte to be read */
    uint32_t count;             /* bytes queued; invariant: equals the sum of the records' lengths */
    unixseg_t segs[UNIX_MAX_SEGS];
    int seg_head, seg_count;

    struct unixsock *backlog[UNIX_BACKLOG];
    int backlog_count;
} unixsock_t;

/* Bound names. A byte string with an explicit length rather than a C
 * string, because an abstract name begins with a NUL and k_strcmp would
 * call every one of them equal. */
typedef struct {
    int len;                    /* 0 when the entry is free */
    char name[UNIX_PATH_MAX];
    unixsock_t *sock;
} unixname_t;

static unixname_t names[UNIX_MAX_NAMES];
static int live_count;
static int queued_fd_count; /* descriptors sitting in records nobody has read */

void unixsock_init(void) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        names[i].len = 0;
        names[i].sock = (unixsock_t *)0;
    }
    live_count = 0;
    queued_fd_count = 0;
    spin_unlock_irqrestore(&unix_lock, f);
}

/* One wake channel rather than pipe.c's two, and that is a decision with
 * a measurement behind it - somebody else's. pipe.c splits "data" from
 * "space" because the window-manager protocol runs through pipes
 * hundreds of times a second and waking a writer once per byte the
 * reader took was visible. Nothing runs through this yet, so it uses the
 * channel every other wait in the kernel already watches: a wake for
 * somebody else costs one more trip round a loop that re-asks, and
 * splitting it when there is something to measure is four lines. M69's
 * rule, applied to a file that has no users yet. */
static void unix_wake(void) {
    sched_wake_all(SCHED_POLL_CHAN);
}

static unixsock_t *unix_new(int type) {
    if (type != UNIX_SOCK_STREAM && type != UNIX_SOCK_SEQPACKET) {
        return (unixsock_t *)0;
    }
    if (live_count >= UNIX_MAX_SOCKETS) {
        return (unixsock_t *)0; /* refused with a number behind it - see the header */
    }
    unixsock_t *s = (unixsock_t *)kmalloc(sizeof(unixsock_t));
    if (!s) {
        return (unixsock_t *)0;
    }
    /* k_memset rather than field-by-field: this object is mostly a 4 KiB
     * buffer and an array of records, and "every field starts at zero" is
     * the whole of its initial state. */
    k_memset(s, 0, sizeof(*s));
    s->type = type;
    s->refs = 1;
    live_count++;
    return s;
}

struct unixsock *unixsock_alloc(int type) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    unixsock_t *s = unix_new(type);
    spin_unlock_irqrestore(&unix_lock, f);
    return s;
}

int unixsock_pair(int type, struct unixsock **a_out, struct unixsock **b_out) {
    if (!a_out || !b_out) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    unixsock_t *a = unix_new(type);
    unixsock_t *b = unix_new(type);
    if (!a || !b) {
        /* Either both or neither: a pair with one end is not a socket a
         * caller could do anything with, and freeing here is safe
         * because nothing else can have seen them. */
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

int unixsock_type(const struct unixsock *s) {
    return s ? s->type : -1;
}

void unixsock_ref(struct unixsock *s) {
    if (!s) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    s->refs++;
    spin_unlock_irqrestore(&unix_lock, f);
}

/* Drops every descriptor `s` still holds in its unread records. Called
 * with the lock NOT held, on an object no one else can reach any more -
 * see the lock note at the top for why it cannot be done under it.
 *
 * This is also where this file recurses: one of those descriptors may be
 * another Unix-domain socket, whose own unref runs this again. The depth
 * is bounded and the bound is worth writing down rather than trusting -
 * each frame destroys a distinct object and there are at most
 * UNIX_MAX_SOCKETS (64) of them, at roughly 200 bytes of frame, against
 * a 32 KiB kernel stack with a guard page under it (M81). 13 KiB of 32,
 * worst case, and a panic rather than corruption if that arithmetic is
 * ever wrong. */
static void unix_release_queued_fds(unixsock_t *s) {
    for (int i = 0; i < s->seg_count; i++) {
        unixseg_t *seg = &s->segs[(s->seg_head + i) % UNIX_MAX_SEGS];
        for (int j = 0; j < seg->nfds; j++) {
            fd_release(&seg->fds[j]);
        }
        seg->nfds = 0;
    }
    s->seg_count = 0;
}

void unixsock_unref(struct unixsock *s) {
    if (!s) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (--s->refs > 0) {
        spin_unlock_irqrestore(&unix_lock, f);
        return;
    }
    /* Last reference. Detach from everything that could still find this
     * object, under the lock, and only then take it apart. */
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        if (names[i].sock == s) {
            names[i].len = 0;
            names[i].sock = (unixsock_t *)0;
        }
    }
    if (s->peer) {
        /* The peer learns the truth, which is that this direction is
         * over: its reads return end-of-stream and its sends fail. Doing
         * this here rather than leaving a dangling pointer is the whole
         * of why a peer link is a pointer and not an id. */
        s->peer->peer = (unixsock_t *)0;
        s->peer = (unixsock_t *)0;
    }
    unixsock_t *pending[UNIX_BACKLOG];
    int npending = s->backlog_count;
    for (int i = 0; i < npending; i++) {
        pending[i] = s->backlog[i];
    }
    s->backlog_count = 0;
    for (int i = 0; i < s->seg_count; i++) {
        queued_fd_count -= s->segs[(s->seg_head + i) % UNIX_MAX_SEGS].nfds;
    }
    live_count--;
    spin_unlock_irqrestore(&unix_lock, f);

    unix_wake();

    /* Private now: no name names it, no peer points at it, no fd slot
     * holds it. The two loops below reach into pipe.c and kernel/fs and
     * into this function again, and both are safe precisely because the
     * lock is not held. */
    unix_release_queued_fds(s);
    for (int i = 0; i < npending; i++) {
        unixsock_unref(pending[i]); /* a connection nobody ever accepted */
    }
    kfree(s);
}

/* ---- names ------------------------------------------------------------ */

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

static unixsock_t *name_lookup(const char *name, int len) {
    for (int i = 0; i < UNIX_MAX_NAMES; i++) {
        if (names[i].len && name_eq(&names[i], name, len)) {
            return names[i].sock;
        }
    }
    return (unixsock_t *)0;
}

int unixsock_bind(struct unixsock *s, const char *name, int len) {
    if (!s || !name || len <= 0 || len > UNIX_PATH_MAX) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->bound || s->peer) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1; /* a socket has one name, and a connected one has no use for one */
    }
    if (name_lookup(name, len)) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1; /* EADDRINUSE */
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
    return -1; /* the table is full, which is a refusal and not a hang */
}

int unixsock_listen(struct unixsock *s) {
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

int unixsock_connect(struct unixsock *s, const char *name, int len) {
    if (!s || !name || len <= 0 || len > UNIX_PATH_MAX) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->peer || s->listening) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1; /* already connected, or is a server */
    }
    unixsock_t *listener = name_lookup(name, len);
    if (!listener || !listener->listening || listener->type != s->type ||
        listener->backlog_count >= UNIX_BACKLOG) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    /* The server's end of this connection is a NEW socket, which is what
     * makes a listener able to accept a second one. It carries the
     * backlog's reference until accept() takes it over; if nobody ever
     * accepts, the listener's own destruction drops it. */
    unixsock_t *srv = unix_new(s->type);
    if (!srv) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1;
    }
    srv->peer = s;
    s->peer = srv;
    listener->backlog[listener->backlog_count++] = srv;
    spin_unlock_irqrestore(&unix_lock, f);
    unix_wake(); /* the listener may be parked in accept() */
    return 0;
}

struct unixsock *unixsock_accept(struct unixsock *listener) {
    if (!listener) {
        return (unixsock_t *)0;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (!listener->listening || listener->backlog_count == 0) {
        spin_unlock_irqrestore(&unix_lock, f);
        return (unixsock_t *)0;
    }
    unixsock_t *conn = listener->backlog[0];
    for (int i = 1; i < listener->backlog_count; i++) {
        listener->backlog[i - 1] = listener->backlog[i];
    }
    listener->backlog_count--;
    spin_unlock_irqrestore(&unix_lock, f);
    return conn; /* the backlog's reference, handed over rather than added to */
}

/* ---- the ring --------------------------------------------------------- */

static void ring_write(unixsock_t *d, const uint8_t *src, uint32_t len) {
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

static void ring_read(unixsock_t *s, uint8_t *dst, uint32_t len) {
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

/* Drops `len` bytes off the front without copying them anywhere - what a
 * SEQPACKET message longer than the caller's buffer does with its tail,
 * because POSIX says the remainder is discarded and MSG_TRUNC says so. */
static void ring_discard(unixsock_t *s, uint32_t len) {
    s->head = (s->head + len) % UNIX_BUF_SIZE;
    s->count -= len;
}

long unixsock_send(struct unixsock *s, const uint8_t *data, uint32_t len,
                   const fd_slot_t *fds, int nfds) {
    if (!s || nfds < 0 || nfds > UNIX_MAX_FDS || (nfds > 0 && !fds)) {
        return -1;
    }
    /* ---- the references are taken BEFORE the lock, and that is a fix --
     *
     * The obvious shape is to copy each slot into the record and
     * fd_retain it there, under the lock. It deadlocks, and the case is
     * exactly the one this milestone exists for: a descriptor being
     * passed may itself be a Unix-domain socket - Mojo passes channel
     * endpoints over channels - and fd_retain on one calls
     * unixsock_ref, which takes this same non-recursive lock. Found by
     * reading the call graph rather than by a hang, which is the only
     * way this one would have been found: it needs a program that passes
     * a socket over a socket, and the first such program would have been
     * somebody else's.
     *
     * So the retain happens here, against the caller's own live
     * descriptors, and the failure paths below release what this did not
     * end up keeping. A reference taken and given back costs two atomic
     * increments on a path that is already refusing. */
    fd_slot_t kept[UNIX_MAX_FDS];
    for (int i = 0; i < nfds; i++) {
        kept[i] = fds[i];
        fd_retain(&kept[i]);
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    unixsock_t *d = s->peer;
    long fail = -1;        /* EPIPE and friends */
    int refuse = 0;
    if (!d || s->shut_wr || d->shut_rd) {
        refuse = 1; /* EPIPE: there is nobody to receive this, now or later */
    } else if (s->type == UNIX_SOCK_SEQPACKET && len > UNIX_BUF_SIZE) {
        refuse = 1; /* EMSGSIZE: a record that can never fit is not a wait, it is a refusal */
    }
    if (refuse) {
        spin_unlock_irqrestore(&unix_lock, f);
        for (int i = 0; i < nfds; i++) {
            fd_release(&kept[i]);
        }
        return fail;
    }
    uint32_t space = UNIX_BUF_SIZE - d->count;
    uint32_t take = len > space ? space : len;
    if (s->type == UNIX_SOCK_SEQPACKET && take < len) {
        take = 0; /* all or nothing: a message is not chunked */
    }
    /* A new record is needed unless this is plain stream bytes landing
     * behind plain stream bytes. Descriptors always start one, and never
     * land in a record that already has some: they have to arrive with
     * the bytes they were sent with (see unixsock_recv). */
    unixseg_t *tail = d->seg_count ? &d->segs[(d->seg_head + d->seg_count - 1) % UNIX_MAX_SEGS]
                                   : (unixseg_t *)0;
    int need_seg = (s->type == UNIX_SOCK_SEQPACKET) || nfds > 0 || !tail || tail->nfds > 0;
    /* "Nothing fits" is a wait and not an error, and the two ways to get
     * there are a full buffer and a full record queue. A zero-length
     * send carrying descriptors is a real message and is not one of
     * them - it is how an IPC layer hands over a handle with no payload
     * - so it is only blocked when the queue has no room for its
     * record. */
    int would_block = (need_seg && d->seg_count >= UNIX_MAX_SEGS) || (len > 0 && take == 0);
    if (would_block) {
        spin_unlock_irqrestore(&unix_lock, f);
        for (int i = 0; i < nfds; i++) {
            fd_release(&kept[i]);
        }
        return 0;
    }
    if (len == 0 && nfds == 0) {
        spin_unlock_irqrestore(&unix_lock, f);
        return 0; /* write(fd, buf, 0), which means what it means everywhere */
    }
    if (need_seg) {
        unixseg_t *seg = &d->segs[(d->seg_head + d->seg_count) % UNIX_MAX_SEGS];
        seg->len = take;
        seg->nfds = nfds;
        for (int i = 0; i < nfds; i++) {
            seg->fds[i] = kept[i]; /* already retained - see the note above */
        }
        d->seg_count++;
        queued_fd_count += nfds;
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

long unixsock_recv(struct unixsock *s, uint8_t *out, uint32_t max,
                   fd_slot_t *fds_out, int max_fds, int *nfds_out,
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
    fd_slot_t dropped[UNIX_MAX_FDS];
    int ndropped = 0;
    uint64_t f = spin_lock_irqsave(&unix_lock);
    if (s->shut_rd) {
        spin_unlock_irqrestore(&unix_lock, f);
        return -1; /* this direction was shut down here: end of stream */
    }
    if (s->count == 0 && s->seg_count == 0) {
        int eof = (!s->peer || s->peer->shut_wr);
        spin_unlock_irqrestore(&unix_lock, f);
        return eof ? -1 : 0;
    }
    uint32_t got = 0;
    int took_fds = 0;
    while (s->seg_count > 0) {
        unixseg_t *seg = &s->segs[s->seg_head];
        if (seg->nfds > 0) {
            if (took_fds) {
                break; /* the next record's descriptors belong to the next read */
            }
            int n = seg->nfds;
            int keep = n < max_fds ? n : max_fds;
            for (int i = 0; i < keep; i++) {
                fds_out[i] = seg->fds[i];
            }
            /* Descriptors with nowhere to go are CLOSED, not left queued.
             * Linux does the same, and the alternative is worse than
             * losing them: a descriptor the receiver was never told about
             * is a reference nothing can release until the socket dies. */
            for (int i = keep; i < n; i++) {
                dropped[ndropped++] = seg->fds[i];
            }
            queued_fd_count -= n;
            seg->nfds = 0;
            if (nfds_out) {
                *nfds_out = keep;
            }
            if (keep < n && flags_out) {
                *flags_out |= UNIX_RECV_CTRUNC;
            }
            took_fds = 1;
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
            /* The rest of this message does not fit and does not wait:
             * one recv, one message. */
            ring_discard(s, seg->len);
            seg->len = 0;
            s->seg_head = (s->seg_head + 1) % UNIX_MAX_SEGS;
            s->seg_count--;
            if (flags_out) {
                *flags_out |= UNIX_RECV_TRUNC;
            }
        }
        if (s->type == UNIX_SOCK_SEQPACKET) {
            break; /* exactly one record, however much room is left */
        }
        if (got >= max) {
            break;
        }
        /* A stream read keeps going into the next record, unless that
         * record carries descriptors - see the header. */
        if (s->seg_count > 0 && s->segs[s->seg_head].nfds > 0) {
            break;
        }
    }
    spin_unlock_irqrestore(&unix_lock, f);

    /* Outside the lock, for the reason at the top of this file. */
    for (int i = 0; i < ndropped; i++) {
        fd_release(&dropped[i]);
    }
    if (got > 0 || took_fds) {
        unix_wake(); /* space, and a sender may be parked on it */
        return (long)got;
    }
    return 0;
}

int unixsock_pending(const struct unixsock *s) {
    if (!s) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int ready;
    if (s->listening) {
        ready = s->backlog_count > 0;
    } else {
        /* End of stream counts as ready, which is the line that stops a
         * blocking wait from becoming a hang (M68's lesson on pipes,
         * which cost a boot to learn). */
        ready = s->count > 0 || s->seg_count > 0 || !s->peer || s->peer->shut_wr ||
                s->shut_rd;
    }
    spin_unlock_irqrestore(&unix_lock, f);
    return ready ? 1 : 0;
}

int unixsock_shutdown(struct unixsock *s, int how) {
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
    unix_wake(); /* the peer is reading and has to be told there will be no more */
    return 0;
}

int unixsock_in_use(void) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int n = live_count;
    spin_unlock_irqrestore(&unix_lock, f);
    return n;
}

int unixsock_queued_fds(void) {
    uint64_t f = spin_lock_irqsave(&unix_lock);
    int n = queued_fd_count;
    spin_unlock_irqrestore(&unix_lock, f);
    return n;
}
