/* kernel/ipc/epoll.h
 *
 * M119: an interest set that outlives the call - which is the whole of
 * what epoll is, and the reason `poll` is not a substitute for it.
 *
 * ---- what this is for -------------------------------------------------
 *
 * docs/browser.md's second condition: "an epoll-shaped readiness
 * interface, plus `eventfd`/`timerfd`. The message pump is not optional
 * and `poll` is not what it calls." Chromium's `base::MessagePumpEpoll`
 * calls `epoll_create1`, `epoll_ctl` and `epoll_wait` by name and has no
 * `poll` path any more. So does libevent, and so does every other event
 * loop written in the last twenty years.
 *
 * ---- what epoll actually buys, stated rather than assumed -------------
 *
 * Not speed. The usual argument for epoll over poll is O(1) against
 * O(n), and on this machine that argument is worth nothing: MAX_FDS is
 * 128, a scan of 128 slots is a few hundred cycles, and M69's rule says
 * performance work on an unmeasured path does not get done here. This
 * file scans its set linearly and says so.
 *
 * What it buys is three things poll genuinely cannot express:
 *
 *   1. **A set the kernel remembers.** `poll` is handed its whole array
 *      on every call, which means a program with one descriptor it cares
 *      about and one it is waiting to write to rebuilds that array every
 *      pass. A pump registers once.
 *   2. **A cookie.** `epoll_event.data` comes back with the event, so a
 *      pump maps a ready descriptor to its handler without a side table.
 *      That is the feature ported code leans on hardest.
 *   3. **Edge triggering and one-shot.** Both are statements about what
 *      happened *since the last call*, which a stateless interface cannot
 *      make at all.
 *
 * ---- the seam that makes this testable --------------------------------
 *
 * Nothing in this file knows what a descriptor is. `epoll_scan` takes a
 * function that answers "what is the current event mask for this fd", and
 * syscall.c passes the one implementation the kernel has
 * (`fd_epoll_mask`, beside the one SYS_waitfds uses). So the set
 * management - add, modify, delete, a full table, a stale entry, edge
 * against level, one-shot - is a pure function of the set and the
 * answers, and tests/test_epoll.c drives all of it with a scripted
 * readiness table and no machine under it.
 */
#pragma once

#include <stdint.h>

/* Live epoll sets, and registrations per set. 32 watched descriptors
 * against a MAX_FDS of 128: a set cannot be asked to watch more than a
 * quarter of what a process can hold, which is a refusal with a number
 * behind it (ENOSPC) rather than a table that grows. A pump watches a
 * handful. */
#define EPOLL_MAX       32
#define EPOLL_MAX_WATCH 32

/* The events, with Linux's values - one constant meaning one thing across
 * the ABI, as UNIX_SOCK_SEQPACKET is. The ones this kernel can actually
 * raise are IN, OUT, ERR, HUP and RDHUP; PRI is defined because ported
 * code names it and is never set, for the reason <poll.h> argues at
 * length about POLLPRI (M99 reversed the original decision there: an
 * absent constant is not a smaller failure than an unraised one, it is a
 * compile error). */
#define EPOLLIN        0x001u
#define EPOLLPRI       0x002u
#define EPOLLOUT       0x004u
#define EPOLLERR       0x008u
#define EPOLLHUP       0x010u
#define EPOLLRDHUP     0x2000u
#define EPOLLEXCLUSIVE 0x10000000u /* accepted and ignored: one waiter per set here */
#define EPOLLWAKEUP    0x20000000u /* accepted and ignored: there is no suspend to block */
#define EPOLLONESHOT   0x40000000u
#define EPOLLET        0x80000000u

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

struct epoll;

/* What comes out of a scan, and what the syscall copies to user space.
 * `data` is the caller's cookie, carried and never interpreted. */
typedef struct {
    uint32_t events;
    uint32_t reserved;
    uint64_t data;
} epoll_ev_t;

void epoll_init(void);

struct epoll *epoll_create_set(void);
void epoll_ref(struct epoll *ep);
void epoll_unref(struct epoll *ep);

/* ADD, MOD or DEL. `fd` is the descriptor number and `obj` is what that
 * descriptor pointed at when the call was made - both are stored, and
 * both are checked on every scan.
 *
 * **Why the object pointer is part of the identity.** On Linux a
 * registration refers to the open *file*, and closing the descriptor
 * removes it from the set. This file cannot hold a reference to the
 * object - that would keep a pipe alive because somebody forgot to
 * deregister it, which is a leak wearing an epoll set as a disguise - so
 * instead it remembers what the fd pointed at and drops the registration
 * the moment the fd stops pointing there. A closed fd, or one reused by a
 * later open, is a stale entry and is deleted by the next scan rather
 * than reported against whatever now lives in that slot. That is the one
 * divergence from Linux in this file and it is the conservative
 * direction: Linux would keep reporting the old file until the
 * registration was removed.
 *
 * Returns 0, or -1 (ADD of a descriptor already in the set, MOD or DEL of
 * one that is not, a full set, an unknown op). */
int epoll_ctl_set(struct epoll *ep, int op, int fd, const void *obj,
                  uint32_t events, uint64_t data);

/* How many descriptors are registered - for the leak audit and for
 * `epoll_ctl`'s own "is it in there" answers. */
int epoll_watch_count(const struct epoll *ep);

/* The current mask for a registration, answered by syscall.c. `obj` is
 * what was registered, so the callback can notice the descriptor no
 * longer points there (it returns EPOLL_STALE for that case) without this
 * file knowing what any of it means. */
#define EPOLL_STALE 0xFFFFFFFFu
typedef uint32_t (*epoll_mask_fn)(void *ctx, int fd, const void *obj);

/* Fills `out` with up to `max` ready registrations and returns how many.
 *
 * Level-triggered by default: a descriptor that is still readable is
 * reported again on the next scan. EPOLLET reports it only when the mask
 * changes to include something it did not before, which is what "edge"
 * means and is why this file keeps a `last` per registration.
 * EPOLLONESHOT disarms the registration after one report - it stays in
 * the set and reports nothing until a MOD re-arms it, which is how a pump
 * hands a descriptor to a worker thread without racing itself.
 *
 * ERR and HUP are reported whether or not they were asked for, as Linux
 * does: a program that did not ask about an error still has to be told,
 * and the alternative is a loop that waits forever on a descriptor whose
 * peer is gone. */
int epoll_scan(struct epoll *ep, epoll_mask_fn mask_fn, void *ctx,
               epoll_ev_t *out, int max);

int epoll_in_use(void);
