/* kernel/ipc/epoll.c - M119. See epoll.h, including why the readiness
 * question is a function pointer. */
#include "epoll.h"

#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/heap.h"

static spinlock_t epoll_lock;

typedef struct {
    int used;
    int fd;
    const void *obj;  /* what that fd pointed at when it was registered - see epoll.h */
    uint32_t events;  /* what the caller asked about, flags included */
    uint64_t data;    /* the caller's cookie, carried and never interpreted */
    uint32_t last;    /* the mask last reported, for EPOLLET */
    int disarmed;     /* EPOLLONESHOT has fired; a MOD re-arms it */
} epoll_watch_t;

typedef struct epoll {
    int refs;
    epoll_watch_t w[EPOLL_MAX_WATCH];
} epoll_t;

static int live_count;

void epoll_init(void) {
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    live_count = 0;
    spin_unlock_irqrestore(&epoll_lock, f);
}

struct epoll *epoll_create_set(void) {
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    if (live_count >= EPOLL_MAX) {
        spin_unlock_irqrestore(&epoll_lock, f);
        return (epoll_t *)0;
    }
    epoll_t *ep = (epoll_t *)kmalloc(sizeof(epoll_t));
    if (!ep) {
        spin_unlock_irqrestore(&epoll_lock, f);
        return (epoll_t *)0;
    }
    k_memset(ep, 0, sizeof(*ep));
    ep->refs = 1;
    live_count++;
    spin_unlock_irqrestore(&epoll_lock, f);
    return ep;
}

void epoll_ref(struct epoll *ep) {
    if (!ep) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    ep->refs++;
    spin_unlock_irqrestore(&epoll_lock, f);
}

void epoll_unref(struct epoll *ep) {
    if (!ep) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    int gone = (--ep->refs <= 0);
    if (gone) {
        live_count--;
    }
    spin_unlock_irqrestore(&epoll_lock, f);
    if (gone) {
        /* A set holds no references to what it watches, deliberately (see
         * epoll.h), so there is nothing to release here - which is the
         * whole point of that decision rather than a convenience. */
        kfree(ep);
    }
}

static epoll_watch_t *find(epoll_t *ep, int fd) {
    for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
        if (ep->w[i].used && ep->w[i].fd == fd) {
            return &ep->w[i];
        }
    }
    return (epoll_watch_t *)0;
}

int epoll_ctl_set(struct epoll *ep, int op, int fd, const void *obj,
                  uint32_t events, uint64_t data) {
    if (!ep || fd < 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    epoll_watch_t *w = find(ep, fd);
    int rc = -1;
    switch (op) {
    case EPOLL_CTL_ADD:
        if (w) {
            /* EEXIST, and it is worth refusing rather than treating as a
             * MOD: a pump that adds twice has lost track of its own set,
             * and the second registration's data would silently replace
             * the first one's handler. */
            break;
        }
        for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
            if (!ep->w[i].used) {
                ep->w[i].used = 1;
                ep->w[i].fd = fd;
                ep->w[i].obj = obj;
                ep->w[i].events = events;
                ep->w[i].data = data;
                /* A fresh registration has reported nothing, so an
                 * edge-triggered one fires on the first scan if the
                 * descriptor is already ready. That is Linux's behaviour
                 * and the only safe one: a program that registers a
                 * socket which already has data and is never told would
                 * wait for a second arrival that may not come. */
                ep->w[i].last = 0;
                ep->w[i].disarmed = 0;
                rc = 0;
                break;
            }
        }
        break;
    case EPOLL_CTL_MOD:
        if (!w) {
            break; /* ENOENT */
        }
        w->events = events;
        w->data = data;
        w->obj = obj; /* the fd may legitimately be a different object now - a MOD is a re-registration */
        w->last = 0;
        w->disarmed = 0; /* what re-arms a one-shot, and the reason MOD exists in a pump at all */
        rc = 0;
        break;
    case EPOLL_CTL_DEL:
        if (!w) {
            break;
        }
        w->used = 0;
        w->obj = (const void *)0;
        rc = 0;
        break;
    default:
        break;
    }
    spin_unlock_irqrestore(&epoll_lock, f);
    return rc;
}

int epoll_watch_count(const struct epoll *ep) {
    if (!ep) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    int n = 0;
    for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
        if (ep->w[i].used) {
            n++;
        }
    }
    spin_unlock_irqrestore(&epoll_lock, f);
    return n;
}

int epoll_scan(struct epoll *ep, epoll_mask_fn mask_fn, void *ctx,
               epoll_ev_t *out, int max) {
    if (!ep || !mask_fn || !out || max <= 0) {
        return 0;
    }
    int n = 0;
    /* ---- the lock is NOT held across the callback ---------------------
     *
     * mask_fn reaches into pipes, sockets and the Unix-domain sockets of
     * M118, each of which takes its own lock - so calling it under this
     * one would make epoll_lock an outer lock over three others for no
     * reason, and would make the lock order depend on which kind of
     * descriptor somebody happened to register. The registration is
     * copied out, the lock is dropped, the question is asked, and the
     * lock is retaken to write `last` back - re-checking that the entry is
     * still the one that was copied, because a concurrent epoll_ctl may
     * have deleted it while this was asking about it.
     *
     * What that costs is honest: a set edited while it is being scanned
     * can report one event for a registration that was deleted a
     * microsecond ago. A pump's answer to that is the same as on Linux,
     * where the race also exists - it looks the descriptor up in its own
     * table and finds nothing. */
    for (int i = 0; i < EPOLL_MAX_WATCH && n < max; i++) {
        uint64_t f = spin_lock_irqsave(&epoll_lock);
        if (!ep->w[i].used || ep->w[i].disarmed) {
            spin_unlock_irqrestore(&epoll_lock, f);
            continue;
        }
        epoll_watch_t snap = ep->w[i];
        spin_unlock_irqrestore(&epoll_lock, f);

        uint32_t mask = mask_fn(ctx, snap.fd, snap.obj);
        if (mask == EPOLL_STALE) {
            /* The descriptor was closed, or now points at something else.
             * The registration goes, rather than being reported against
             * whatever took the slot - see epoll.h. */
            f = spin_lock_irqsave(&epoll_lock);
            if (ep->w[i].used && ep->w[i].fd == snap.fd && ep->w[i].obj == snap.obj) {
                ep->w[i].used = 0;
                ep->w[i].obj = (const void *)0;
            }
            spin_unlock_irqrestore(&epoll_lock, f);
            continue;
        }
        /* What the caller asked about, plus the two it is told about
         * whether it asked or not. */
        uint32_t want = (snap.events & (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDHUP)) |
                        EPOLLERR | EPOLLHUP;
        uint32_t hit = mask & want;
        if (hit == 0) {
            if (snap.last != 0) {
                f = spin_lock_irqsave(&epoll_lock);
                if (ep->w[i].used && ep->w[i].fd == snap.fd) {
                    ep->w[i].last = 0; /* an edge-triggered registration re-arms when the condition goes away */
                }
                spin_unlock_irqrestore(&epoll_lock, f);
            }
            continue;
        }
        if (snap.events & EPOLLET) {
            /* Edge: only what is newly set since the last report. A
             * descriptor that was readable and still is reports nothing;
             * one that becomes writable as well reports the writability. */
            uint32_t fresh = hit & ~snap.last;
            if (fresh == 0) {
                continue;
            }
            hit = fresh;
        }
        out[n].events = hit;
        out[n].reserved = 0;
        out[n].data = snap.data;
        n++;
        f = spin_lock_irqsave(&epoll_lock);
        if (ep->w[i].used && ep->w[i].fd == snap.fd && ep->w[i].obj == snap.obj) {
            ep->w[i].last |= hit;
            if (snap.events & EPOLLONESHOT) {
                ep->w[i].disarmed = 1;
            }
        }
        spin_unlock_irqrestore(&epoll_lock, f);
    }
    return n;
}

int epoll_in_use(void) {
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    int n = live_count;
    spin_unlock_irqrestore(&epoll_lock, f);
    return n;
}
