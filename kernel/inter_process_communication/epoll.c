#include "epoll.h"

#include "drivers/kernel_log.h"
#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "syscall.h"

static spinlock_t epoll_lock;

typedef struct {
    int used;
    int fd;
    const void *object;
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
static int refusals;

/* A ceiling reached says so, once and then every 64th time, because what the
   program sees is only "it failed" (M183's rule, met again in M187). */
static void refused(const char *what) {
    refusals++;
    if (refusals == 1 || refusals % 64 == 0) {
        kernel_log_puts("[epoll] refused: ");
        kernel_log_puts(what);
        kernel_log_puts(" (");
        kernel_log_put_dec((uint32_t)refusals);
        kernel_log_puts(" refusal(s) since boot)\n");
    }
}

void epoll_init(void) {
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    live_count = 0;
    spin_unlock_irqrestore(&epoll_lock, f);
}

struct epoll *epoll_create_set(void) {
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    if (live_count >= EPOLL_MAX) {
        spin_unlock_irqrestore(&epoll_lock, f);
        refused("the machine is at its ceiling of epoll sets");
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

void epoll_reference(struct epoll *ep) {
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

int epoll_control_set(struct epoll *ep, int op, int fd, const void *object,
                  uint32_t events, uint64_t data) {
    if (!ep || fd < 0) {
        return -OS_ERROR_INVALID;
    }
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    epoll_watch_t *w = find(ep, fd);
    /* M226: each refusal is Linux's own errno, not one -1 for all of them -
       libuv's loop retries an ADD that says EEXIST as a MOD, and asserts
       that is what it was told. */
    int rc = -OS_ERROR_INVALID;
    int full = 0;
    /* A watch is kept by descriptor NUMBER, and closing a descriptor does not
       take it out of the sets watching it - on Linux the last close of a
       file removes it from every epoll set, and here nothing does. So a watch
       whose object is not the one the number names now belongs to a file
       that was closed, and the number has been handed to something else.

       It used to be that watch that answered: EPOLL_CTL_ADD for the new file
       was refused as a duplicate, the next scan discarded the old watch as
       stale, and the new file was watched by nobody. Chromium closes and
       opens a mojo channel socket at a time and reuses the lowest number, so
       its browser process ended up with messages from its children readable
       and every thread asleep - a sync call to its own GPU thread that never
       returned, and a window that never drew (M187). The closed file's watch
       is not a registration of this one, and this one is what is asked
       about. */
    if (w && object && w->object && w->object != object) {
        if (op == EPOLL_CTL_ADD) {
            w->used = 0;
            w->object = (const void *)0;
        }
        w = (epoll_watch_t *)0;
    }
    switch (op) {
    case EPOLL_CTL_ADD:
        if (w) {
            rc = -OS_ERROR_EXIST;
            break;
        }
        full = 1;
        for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
            if (!ep->w[i].used) {
                ep->w[i].used = 1;
                ep->w[i].fd = fd;
                ep->w[i].object = object;
                ep->w[i].events = events;
                ep->w[i].data = data;
                ep->w[i].last = 0;
                ep->w[i].disarmed = 0;
                rc = 0;
                full = 0;
                break;
            }
        }
        break;
    case EPOLL_CTL_MOD:
        if (!w) {
            rc = -OS_ERROR_NOENT;
            break;
        }
        w->events = events;
        w->data = data;
        w->object = object;
        w->last = 0;
        w->disarmed = 0;
        rc = 0;
        break;
    case EPOLL_CTL_DEL:
        if (!w) {
            rc = -OS_ERROR_NOENT;
            break;
        }
        w->used = 0;
        w->object = (const void *)0;
        rc = 0;
        break;
    default:
        break;
    }
    spin_unlock_irqrestore(&epoll_lock, f);
    if (full) {
        refused("an epoll set is at its ceiling of watched descriptors");
        rc = -OS_ERROR_NOSPC;
    }
    return rc;
}

int epoll_objects(struct epoll *ep, int *fds, const void **objects, int max) {
    if (!ep) {
        return 0;
    }
    int n = 0;
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    for (int i = 0; i < EPOLL_MAX_WATCH && n < max; i++) {
        if (ep->w[i].used && !ep->w[i].disarmed) {
            fds[n] = ep->w[i].fd;
            objects[n] = ep->w[i].object;
            n++;
        }
    }
    spin_unlock_irqrestore(&epoll_lock, f);
    return n;
}

/* Every registration, the disarmed ones too: EPOLL_CTL_MOD re-arms a
   one-shot watch without asking anything, so a question about what a set can
   reach (M226, nested sets) has to count the watches that are only asleep. */
int epoll_registered(struct epoll *ep, int *fds, const void **objects, int max) {
    if (!ep) {
        return 0;
    }
    int n = 0;
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    for (int i = 0; i < EPOLL_MAX_WATCH && n < max; i++) {
        if (ep->w[i].used) {
            fds[n] = ep->w[i].fd;
            objects[n] = ep->w[i].object;
            n++;
        }
    }
    spin_unlock_irqrestore(&epoll_lock, f);
    return n;
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

int epoll_scan(struct epoll *ep, epoll_mask_function mask_function, void *context,
               epoll_ev_t *out, int max) {
    if (!ep || !mask_function || !out || max <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < EPOLL_MAX_WATCH && n < max; i++) {
        /* M204: a set holds a handful of descriptors in 128 slots, and this
           took the machine-wide lock with interrupts off for every slot,
           empty or not - the hottest kernel address while a browser ran.
           An empty slot is passed over on a plain read; the decision is
           still made under the lock below, so a watch added in between is
           found by the next scan, which its own wake asks for. */
        if (!__atomic_load_n(&ep->w[i].used, __ATOMIC_RELAXED)) {
            continue;
        }
        uint64_t f = spin_lock_irqsave(&epoll_lock);
        if (!ep->w[i].used || ep->w[i].disarmed) {
            spin_unlock_irqrestore(&epoll_lock, f);
            continue;
        }
        epoll_watch_t snap = ep->w[i];
        spin_unlock_irqrestore(&epoll_lock, f);

        uint32_t mask = mask_function(context, snap.fd, snap.object);
        if (mask == EPOLL_STALE) {
            f = spin_lock_irqsave(&epoll_lock);
            if (ep->w[i].used && ep->w[i].fd == snap.fd && ep->w[i].object == snap.object) {
                ep->w[i].used = 0;
                ep->w[i].object = (const void *)0;
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
        if (ep->w[i].used && ep->w[i].fd == snap.fd && ep->w[i].object == snap.object) {
            ep->w[i].last |= hit;
            if (snap.events & EPOLLONESHOT) {
                ep->w[i].disarmed = 1;
            }
        }
        spin_unlock_irqrestore(&epoll_lock, f);
    }
    return n;
}

/* M226: whether epoll_scan would report anything, answered without moving
   anything epoll_scan moves - an edge already reported stays reported, a
   one-shot stays armed, a stale watch stays for the scan to drop. This is
   what a set's OWN descriptor says when it is watched (an epoll set inside
   another, which is how Electron joins libuv's loop to Chromium's): Linux
   calls the set readable when its ready list is not empty, and the ready
   list is exactly what the next wait would return. */
int epoll_peek(struct epoll *ep, epoll_mask_function mask_function, void *context) {
    if (!ep || !mask_function) {
        return 0;
    }
    for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
        if (!__atomic_load_n(&ep->w[i].used, __ATOMIC_RELAXED)) {
            continue;
        }
        uint64_t f = spin_lock_irqsave(&epoll_lock);
        if (!ep->w[i].used || ep->w[i].disarmed) {
            spin_unlock_irqrestore(&epoll_lock, f);
            continue;
        }
        epoll_watch_t snap = ep->w[i];
        spin_unlock_irqrestore(&epoll_lock, f);

        uint32_t mask = mask_function(context, snap.fd, snap.object);
        if (mask == EPOLL_STALE) {
            continue;
        }
        uint32_t want = (snap.events & (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDHUP)) |
                        EPOLLERR | EPOLLHUP;
        uint32_t hit = mask & want;
        if (snap.events & EPOLLET) {
            hit &= ~snap.last;
        }
        if (hit != 0) {
            return 1;
        }
    }
    return 0;
}

int epoll_in_use(void) {
    uint64_t f = spin_lock_irqsave(&epoll_lock);
    int n = live_count;
    spin_unlock_irqrestore(&epoll_lock, f);
    return n;
}
