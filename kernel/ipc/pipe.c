#include "pipe.h"

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
    if (!p || p->persistent) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    if (p->readers > 0 && --p->readers == 0) {
        p->read_closed = 1;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
}

void pipe_unref_write(pipe_t *p) {
    if (!p || p->persistent) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    if (p->writers > 0 && --p->writers == 0) {
        p->write_closed = 1;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
}

void pipe_close_read(pipe_t *p) {
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->read_closed = 1;
    spin_unlock_irqrestore(&pipe_lock, f);
}

void pipe_close_write(pipe_t *p) {
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->write_closed = 1;
    spin_unlock_irqrestore(&pipe_lock, f);
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
             *
             * M67: the lock goes down before the switch and comes back
             * after. Nothing about the pipe is assumed to have survived -
             * the loop re-reads read_closed and count from the top, which
             * it had to do anyway and now genuinely has to. */
            spin_unlock_irqrestore(&pipe_lock, f);
            sched_deliver_pending_signal();
            schedule();
            f = spin_lock_irqsave(&pipe_lock);
            continue;
        }
        p->buf[p->head] = src[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
        written++;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
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
            spin_unlock_irqrestore(&pipe_lock, f); /* see pipe_write's note above */
            sched_deliver_pending_signal();
            schedule();
            f = spin_lock_irqsave(&pipe_lock);
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
    return (long)n;
}
