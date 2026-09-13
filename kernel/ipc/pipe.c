#include "pipe.h"

#include "drivers/pit.h"
#include "syscall.h"
#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "sched/sched.h"

static spinlock_t pipe_lock;

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
        sched_wake_all(PIPE_SPACE_CHAN(p));
        sched_wake_all(SCHED_POLL_CHAN);
    }
}

void pipe_unref_write(pipe_t *p) {
    if (!p) {
        return;
    }
    if (p->persistent) {
        sched_wake_all(PIPE_DATA_CHAN(p));
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
        sched_wake_all(PIPE_DATA_CHAN(p));
        sched_wake_all(SCHED_POLL_CHAN);
    }
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

int pipe_write_closed(pipe_t *p) {
    if (!p) {
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    int closed = p->write_closed;
    spin_unlock_irqrestore(&pipe_lock, f);
    return closed;
}

int pipe_read_closed(pipe_t *p) {
    if (!p) {
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    int closed = p->read_closed;
    spin_unlock_irqrestore(&pipe_lock, f);
    return closed;
}

int pipe_writable(pipe_t *p) {
    if (!p) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    int w = p->read_closed || p->count < PIPE_BUF_SIZE;
    spin_unlock_irqrestore(&pipe_lock, f);
    return w;
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

#define MAX_NAMED_PIPES     32

typedef struct {
    char name[NAMED_PIPE_NAME_LEN];
    pipe_t *p;
} named_pipe_t;

static named_pipe_t named_pipes[MAX_NAMED_PIPES];
static int named_pipe_count;

pipe_t *pipe_named(const char *name) {
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
    p->persistent = 1;
    k_strlcpy(named_pipes[named_pipe_count].name, name, NAMED_PIPE_NAME_LEN);
    named_pipes[named_pipe_count].p = p;
    named_pipe_count++;
    spin_unlock_irqrestore(&pipe_lock, f);
    return p;
}

long pipe_write(pipe_t *p, const void *buf, size_t len, int nonblock) {
    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    while (written < len) {
        if (p->read_closed) {
            spin_unlock_irqrestore(&pipe_lock, f);
            return written > 0 ? (long)written : -1;
        }
        if (p->count == PIPE_BUF_SIZE) {
            sched_wake_all(PIPE_DATA_CHAN(p));
            sched_wake_all(SCHED_POLL_CHAN);
            if (nonblock) {
                spin_unlock_irqrestore(&pipe_lock, f);
                return written ? (long)written : -OS_ERR_AGAIN;
            }
            sched_block_on(PIPE_SPACE_CHAN(p), 0, &pipe_lock, &f);
            continue;
        }
        p->buf[p->head] = src[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
        written++;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    sched_wake_all(PIPE_DATA_CHAN(p));
    sched_wake_all(SCHED_POLL_CHAN);
    return (long)written;
}

long pipe_read(pipe_t *p, void *buf, size_t maxlen, int nonblock) {
    uint8_t *dst = (uint8_t *)buf;
    size_t n = 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    while (n < maxlen) {
        if (p->count == 0) {
            if (p->write_closed) {
                spin_unlock_irqrestore(&pipe_lock, f);
                return (long)n;
            }
            if (nonblock) {
                spin_unlock_irqrestore(&pipe_lock, f);
                return n ? (long)n : -OS_ERR_AGAIN;
            }
            sched_block_on(PIPE_DATA_CHAN(p), 0, &pipe_lock, &f);
            if (sched_signal_pending()) {
                spin_unlock_irqrestore(&pipe_lock, f);
                return n ? (long)n : -OS_ERR_INTR;
            }
            continue;
        }
        dst[n] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
        n++;
        if (p->count == 0) {
            break;
        }
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    if (n > 0) {
        sched_wake_all(PIPE_SPACE_CHAN(p));
    }
    return (long)n;
}
