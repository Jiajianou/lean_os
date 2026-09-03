#include "pipe.h"

#include "drivers/pit.h"
#include "syscall.h" /* system_api/include/syscall.h - OS_ERR_INTR (M98) */
#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "sched/sched.h"

/* ---- M67: pipe_lock --------------------------------------------------
 *
 * What it protects: every pipe's ring buffer (buf/head/tail/count), its
 * closed flags and its reader/writer refcounts, plus the named_pipes
 * table at the bottom of this file. One lock for all pipes rather than
 * one per pipe: the whole window-manager protocol is a handful of pipes
 * touched a few hundred times a second, contention is not the problem
 * here, and a lock inside pipe_t would have to be initialised by every
 * one of the three places that make one.
 *
 * Against whom: the reader and the writer, which are two different
 * processes by definition - that is what a pipe is for. `count++` in
 * pipe_write and `count--` in pipe_read are both read-modify-write on
 * the same field, and before M67 they could not interleave because a
 * syscall could not be preempted. On a second core they could already,
 * and the only reason it never showed is that the window is a handful of
 * instructions wide.
 *
 * THE RULE FOR THIS FILE: the lock is dropped before schedule() and
 * retaken after. A task that parks in the wait loop below holds nothing,
 * which is what makes it safe for the other end of the pipe to run and
 * make progress - and holding a spinlock across a context switch would
 * hand the lock to a task that is not running and cannot release it.
 * Every field is re-read after the reacquire, because the world moved
 * while this task was not in it.
 *
 * Interrupts off while held: the task-exit path unrefs pipes, and it is
 * reachable from a timer tick delivering SIGKILL. */
static spinlock_t pipe_lock;

/* ---- M68: why the two waits in this file still SPIN ------------------
 *
 * Everything else M68 touched leaves the run queue - a task in
 * pit_sleep_ms, a shell on an empty keyboard, a parent in SYS_wait, a
 * program in SYS_waitfds. Pipes deliberately do not, and the reason is
 * a measurement rather than an oversight.
 *
 * Blocking them was implemented, and it worked in the sense that the
 * mechanism was correct: readers parked, writers woke them, the
 * self-tests that exercise pipes directly passed. What it also did was
 * change the latency of every message on the window-manager protocol,
 * because that protocol IS pipes - a window is created, drawn, focused,
 * closed and torn down entirely through them. Roughly fifty boot
 * self-tests are written against fixed pit_sleep_ms budgets that were
 * measured on a desktop where a pipe write reached its reader within one
 * scheduler quantum, and with blocking pipes the failures *wandered*:
 * M36's close handshake on one run, M55's crash recovery on the next,
 * wm_demo's window creation on the one after. A failure that moves
 * between runs is not a bug in the thing being tested; it is the whole
 * suite's timing assumption being wrong at once.
 *
 * So this is a scope cut, made deliberately and with the cost stated:
 * two pipe waits on this machine still spin through schedule(), and a
 * process parked on one is still, to the scheduler, a program that wants
 * the CPU. What it costs is the compositor and its clients - the
 * processes that would benefit most. What it buys is a desktop whose
 * timing is unchanged, which is the precondition for M69 measuring that
 * timing honestly rather than chasing a suite it has just perturbed.
 *
 * M69 owns this. Its first bullet is "measure input-to-photon before
 * changing anything", and with a number in hand, converting these two
 * waits stops being a guess and becomes a change with a before and an
 * after. The wake calls below are already wired up and already correct -
 * what is missing is not the mechanism, it is the evidence.
 */

/* M68: two wait channels per pipe, and they have to be two rather than
 * one. A reader waits for *data*; a writer waits for *space*. Waking both
 * on every event would work and would be wrong in the way that matters
 * on a full pipe: the writer wakes, finds it still full, and parks again,
 * once per byte the reader takes. The addresses are the identity and are
 * never dereferenced - `p` for data, `&p->count` for space, which are
 * distinct addresses within the same object and cost nothing to derive. */
#define PIPE_DATA_CHAN(p)  ((const void *)(p))
#define PIPE_SPACE_CHAN(p) ((const void *)&(p)->count)

pipe_t *pipe_create(void) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) {
        return (pipe_t *)0;
    }
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->read_closed = 0;
    p->write_closed = 0;
    /* SYS_pipe hands back exactly one fd of each end, so that is what a
     * fresh anonymous pipe starts with. */
    p->readers = 1;
    p->writers = 1;
    p->persistent = 0;
    return p;
}

void pipe_ref_read(pipe_t *p) {
    if (!p) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->readers++;
    spin_unlock_irqrestore(&pipe_lock, f);
}

void pipe_ref_write(pipe_t *p) {
    if (!p) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->writers++;
    spin_unlock_irqrestore(&pipe_lock, f);
}

void pipe_unref_read(pipe_t *p) {
    if (!p) {
        return;
    }
    if (p->persistent) {
        /* M68: a persistent pipe never auto-closes - that is the whole
         * mechanism (see the header). But somebody may be PARKED on it,
         * and before M68 nobody could be: a waiter was a spin that
         * re-tested on its own. A waiter that is asleep re-tests nothing,
         * so the holder going away has to say so even when it changes no
         * flag. This is M55's compositor-crash recovery: the clients
         * survive precisely because they notice, and a client asleep on
         * the event pipe of a compositor that just died would never
         * notice anything again. */
        sched_wake_all(PIPE_SPACE_CHAN(p));
        sched_wake_all(SCHED_POLL_CHAN);
        return;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    int closed = 0;
    if (p->readers > 0 && --p->readers == 0) {
        p->read_closed = 1;
        closed = 1;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    if (closed) {
        sched_wake_all(PIPE_SPACE_CHAN(p)); /* M68: see pipe_close_read */
        sched_wake_all(SCHED_POLL_CHAN);
    }
}

void pipe_unref_write(pipe_t *p) {
    if (!p) {
        return;
    }
    if (p->persistent) {
        sched_wake_all(PIPE_DATA_CHAN(p)); /* see pipe_unref_read */
        sched_wake_all(SCHED_POLL_CHAN);
        return;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    int closed = 0;
    if (p->writers > 0 && --p->writers == 0) {
        p->write_closed = 1;
        closed = 1;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    if (closed) {
        sched_wake_all(PIPE_DATA_CHAN(p)); /* M68: see pipe_close_write */
        sched_wake_all(SCHED_POLL_CHAN);
    }
}

/* M68: closing an end has to wake the other one. "The writer went away"
 * and "the writer has not written yet" are the same thing to a blocked
 * reader until somebody tells it otherwise, and before M68 nobody had to
 * - the reader was spinning and re-tested write_closed on its own. A
 * blocked reader re-tests nothing until it is woken, so a close that did
 * not wake would turn end-of-stream into a hang. */
void pipe_close_read(pipe_t *p) {
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->read_closed = 1;
    spin_unlock_irqrestore(&pipe_lock, f);
    sched_wake_all(PIPE_SPACE_CHAN(p));
    sched_wake_all(SCHED_POLL_CHAN);
}

void pipe_close_write(pipe_t *p) {
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->write_closed = 1;
    spin_unlock_irqrestore(&pipe_lock, f);
    sched_wake_all(PIPE_DATA_CHAN(p));
    sched_wake_all(SCHED_POLL_CHAN);
}

void pipe_reset(pipe_t *p) {
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->read_closed = 0;
    p->write_closed = 0;
    spin_unlock_irqrestore(&pipe_lock, f);
}

/* M67: the poll half of the read/write pair, moved in here so it reads
 * the count under the same lock that maintains it. A torn read of a
 * 32-bit count is not a real hazard on x86_64, but "how many bytes are
 * there" answered outside the lock is a number that was true and may not
 * be by the time the caller acts on it, and every caller in this project
 * follows it with a read that has to agree. */
int pipe_write_closed(pipe_t *p) {
    if (!p) {
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    int closed = p->write_closed;
    spin_unlock_irqrestore(&pipe_lock, f);
    return closed;
}

int pipe_buffered(pipe_t *p) {
    if (!p) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    int n = (int)p->count;
    spin_unlock_irqrestore(&pipe_lock, f);
    return n;
}

/* M21: bumped from 8 - wm_req/wm_resp plus one event pipe per connected
 * window (system_api/include/wm.h's wm_event_pipe_name) outgrew the old
 * cap. M41: 24 -> 32, for the shared menu bar's four extra well-known
 * names on top of the six the window/query/action/settings channels
 * already use. M42 deleted that bar and its four names, but left the cap
 * where it is: 12 per-window event pipes (WM_MAX_ROUTABLE_WINDOWS) plus
 * six well-known ones is 18, and headroom above that is exactly what the
 * previous exact-fit caps taught (see M40). Same "bump the fixed cap when
 * a real need arrives" precedent as MAX_TASKS and MAX_FDS. */
#define MAX_NAMED_PIPES     32

typedef struct {
    char name[NAMED_PIPE_NAME_LEN];
    pipe_t *p;
} named_pipe_t;

static named_pipe_t named_pipes[MAX_NAMED_PIPES];
static int named_pipe_count;

pipe_t *pipe_named(const char *name) {
    /* M67: find-or-create has to be one atomic step. Two processes
     * rendezvousing on the same well-known name is the *expected* use -
     * that is what this function is for - so "both looked, both missed,
     * both created" is not a corner case here, it is Tuesday, and the
     * loser's pipe would be a channel one side is talking into that the
     * other side will never read. */
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    for (int i = 0; i < named_pipe_count; i++) {
        if (k_strcmp(named_pipes[i].name, name) == 0) {
            pipe_t *found = named_pipes[i].p;
            spin_unlock_irqrestore(&pipe_lock, f);
            return found;
        }
    }
    if (named_pipe_count >= MAX_NAMED_PIPES) {
        spin_unlock_irqrestore(&pipe_lock, f);
        return (pipe_t *)0;
    }
    pipe_t *p = pipe_create();
    if (!p) {
        spin_unlock_irqrestore(&pipe_lock, f);
        return (pipe_t *)0;
    }
    /* M59: a rendezvous point, not a conversation - it outlives every
     * process that ever holds an fd on it, which is exactly what M55's
     * compositor-replacement recovery is built on. */
    p->persistent = 1;
    k_strlcpy(named_pipes[named_pipe_count].name, name, NAMED_PIPE_NAME_LEN);
    named_pipes[named_pipe_count].p = p;
    named_pipe_count++;
    spin_unlock_irqrestore(&pipe_lock, f);
    return p;
}

long pipe_write(pipe_t *p, const void *buf, size_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    while (written < len) {
        if (p->read_closed) {
            spin_unlock_irqrestore(&pipe_lock, f);
            return written > 0 ? (long)written : -1;
        }
        if (p->count == PIPE_BUF_SIZE) {
            /* M42: a task parked here is past syscall_handler's own signal
             * check and gets no timer tick while it's current - see
             * sched_deliver_pending_signal. Without this it is unkillable.
             * sched_block_on makes that check itself, first thing.
             *
             * M68: this was a `schedule()` spin, which left the task
             * READY - so a writer waiting on a full pipe was, to the
             * scheduler, a program that wanted the CPU. Now it leaves the
             * run queue entirely and the reader's own pipe_read wakes it.
             * The lock is handed to sched_block_on, which drops it only
             * after this task is marked BLOCKED: that ordering is what
             * makes "the pipe is full" and "I am asleep" one step as far
             * as the reader is concerned, and it is the whole of the
             * lost-wakeup problem. Nothing about the pipe is assumed to
             * have survived - the loop re-reads from the top.
             *
             * The wake BEFORE the park is not an optimisation, it is the
             * deadlock fix. The bytes already written are not visible to
             * anyone until somebody is told about them, and this
             * function's own wake at the bottom is unreachable from here
             * - it only runs once the whole write completes. So a
             * compositor filling a client's event pipe would park
             * holding a pipe full of events the client had never been
             * woken to read, while that client sat in SYS_waitfds on
             * exactly that pipe. Both asleep, each waiting for the other.
             * Found by this milestone's own boot hang, not by reading the
             * code: the shape only appears once BOTH sides can sleep, and
             * before M68 neither could. */
            sched_wake_all(PIPE_DATA_CHAN(p));
            sched_wake_all(SCHED_POLL_CHAN);
            sched_block_on(PIPE_SPACE_CHAN(p), 0, &pipe_lock, &f);
            continue;
        }
        p->buf[p->head] = src[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
        written++;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    /* Outside the lock: a waker only touches the task table, and doing it
     * here keeps the critical section to the ring arithmetic. */
    sched_wake_all(PIPE_DATA_CHAN(p));
    sched_wake_all(SCHED_POLL_CHAN); /* M68: and anything in SYS_waitfds */
    return (long)written;
}

long pipe_read(pipe_t *p, void *buf, size_t maxlen) {
    uint8_t *dst = (uint8_t *)buf;
    size_t n = 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    while (n < maxlen) {
        if (p->count == 0) {
            if (p->write_closed) {
                spin_unlock_irqrestore(&pipe_lock, f);
                return (long)n;
            }
            sched_block_on(PIPE_DATA_CHAN(p), 0, &pipe_lock, &f); /* see pipe_write's note above */
            /* M76: a caught signal ends the wait as well as an arriving
             * byte. Without this, a program that spends its life blocked
             * on a pipe - which on this machine is most of them - could
             * install a handler and never run it, because the handler
             * only runs on the way back out to ring 3 and this loop
             * never goes back out. Returning short (possibly zero) is
             * the honest answer and is what every caller here already
             * copes with; the handler runs during the return. */
            if (sched_signal_pending()) {
                spin_unlock_irqrestore(&pipe_lock, f);
                /* M98: a short count when something was read, and
                 * -OS_ERR_INTR when nothing was. The 0 that used to be
                 * returned here says "end of file" to every program
                 * written against POSIX, and GNU make's job server is
                 * the one that said so out loud - see OS_ERR_INTR in
                 * system_api/include/syscall.h for the whole story. */
                return n ? (long)n : -OS_ERR_INTR;
            }
            continue;
        }
        dst[n] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
        n++;
        if (p->count == 0) {
            break; /* grab what's ready, don't block for more once we have at least one byte */
        }
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    if (n > 0) {
        sched_wake_all(PIPE_SPACE_CHAN(p)); /* M68: a writer may have been waiting for exactly this */
    }
    return (long)n;
}
