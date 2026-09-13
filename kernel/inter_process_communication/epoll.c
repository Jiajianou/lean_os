#include "epoll.h"

#include "library/libk.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"

static spinlock_t epoll_lock;

typedef struct {
    int used;
    int fd;
    const void *obj;
    uint32_t events;
    uint64_t data;
    uint32_t last;
    int disarmed;
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
            break;
        }
        for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
            if (!ep->w[i].used) {
                ep->w[i].used = 1;
                ep->w[i].fd = fd;
                ep->w[i].obj = obj;
                ep->w[i].events = events;
                ep->w[i].data = data;
                ep->w[i].last = 0;
                ep->w[i].disarmed = 0;
                rc = 0;
                break;
            }
        }
        break;
    case EPOLL_CTL_MOD:
        if (!w) {
            break;
        }
        w->events = events;
        w->data = data;
        w->obj = obj;
        w->last = 0;
        w->disarmed = 0;
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
            f = spin_lock_irqsave(&epoll_lock);
            if (ep->w[i].used && ep->w[i].fd == snap.fd && ep->w[i].obj == snap.obj) {
                ep->w[i].used = 0;
                ep->w[i].obj = (const void *)0;
            }
            spin_unlock_irqrestore(&epoll_lock, f);
            continue;
        }
        uint32_t want = (snap.events & (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDHUP)) |
                        EPOLLERR | EPOLLHUP;
        uint32_t hit = mask & want;
        if (hit == 0) {
            if (snap.last != 0) {
                f = spin_lock_irqsave(&epoll_lock);
                if (ep->w[i].used && ep->w[i].fd == snap.fd) {
                    ep->w[i].last = 0;
                }
                spin_unlock_irqrestore(&epoll_lock, f);
            }
            continue;
        }
        if (snap.events & EPOLLET) {
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
