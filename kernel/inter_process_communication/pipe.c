#include "pipe.h"

#include "drivers/pit.h"
#include "syscall.h"
#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

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

void pipe_reference_read(pipe_t *p) {
    if (!p) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->readers++;
    spin_unlock_irqrestore(&pipe_lock, f);
}

void pipe_reference_write(pipe_t *p) {
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
        scheduler_wake_all(PIPE_SPACE_CHAN(p));
        scheduler_wake_object(p);
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
        scheduler_wake_all(PIPE_SPACE_CHAN(p));
        scheduler_wake_object(p);
    }
}

void pipe_unref_write(pipe_t *p) {
    if (!p) {
        return;
    }
    if (p->persistent) {
        scheduler_wake_all(PIPE_DATA_CHAN(p));
        scheduler_wake_object(p);
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
        scheduler_wake_all(PIPE_DATA_CHAN(p));
        scheduler_wake_object(p);
    }
}

void pipe_close_write(pipe_t *p) {
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    p->write_closed = 1;
    spin_unlock_irqrestore(&pipe_lock, f);
    scheduler_wake_all(PIPE_DATA_CHAN(p));
    scheduler_wake_object(p);
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
    int w = p->read_closed || p->count < PIPE_BUFFER_SIZE;
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
    char name[NAMED_PIPE_NAME_LENGTH];
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
    k_strlcpy(named_pipes[named_pipe_count].name, name, NAMED_PIPE_NAME_LENGTH);
    named_pipes[named_pipe_count].p = p;
    named_pipe_count++;
    spin_unlock_irqrestore(&pipe_lock, f);
    return p;
}

/* POSIX's PIPE_BUF promise, which this is the whole capacity of: a write
   that fits in an empty pipe goes in whole or waits, and a non-blocking one
   goes in whole or not at all. Copying byte by byte until the pipe filled
   broke both halves. Every window on the desktop writes its fixed-size
   request into one shared pipe, so two requests that met a nearly full pipe
   could interleave; and the compositor's events are written without
   blocking, so a client that fell behind would have been sent half an event
   and read every one after it shifted (M209). */
static int pipe_wait_for_room(pipe_t *p, size_t length, int nonblock, uint64_t *f) {
    while (!p->read_closed && PIPE_BUFFER_SIZE - p->count < length) {
        scheduler_wake_all(PIPE_DATA_CHAN(p));
        scheduler_wake_object(p);
        if (nonblock) {
            return -OS_ERROR_AGAIN;
        }
        scheduler_block_on(PIPE_SPACE_CHAN(p), 0, &pipe_lock, f);
    }
    return 0;
}

long pipe_write(pipe_t *p, const void *buffer, size_t length, int nonblock) {
    const uint8_t *source = (const uint8_t *)buffer;
    size_t written = 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    if (length <= PIPE_BUFFER_SIZE) {
        int waited = pipe_wait_for_room(p, length, nonblock, &f);
        if (waited < 0) {
            spin_unlock_irqrestore(&pipe_lock, f);
            return waited;
        }
    }
    while (written < length) {
        if (p->read_closed) {
            spin_unlock_irqrestore(&pipe_lock, f);
            return written > 0 ? (long)written : -1;
        }
        if (p->count == PIPE_BUFFER_SIZE) {
            scheduler_wake_all(PIPE_DATA_CHAN(p));
            scheduler_wake_object(p);
            if (nonblock) {
                spin_unlock_irqrestore(&pipe_lock, f);
                return written ? (long)written : -OS_ERROR_AGAIN;
            }
            scheduler_block_on(PIPE_SPACE_CHAN(p), 0, &pipe_lock, &f);
            continue;
        }
        p->buffer[p->head] = source[written];
        p->head = (p->head + 1) % PIPE_BUFFER_SIZE;
        p->count++;
        written++;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    scheduler_wake_all(PIPE_DATA_CHAN(p));
    scheduler_wake_object(p);
    return (long)written;
}

long pipe_read(pipe_t *p, void *buffer, size_t maxlen, int nonblock) {
    uint8_t *destination = (uint8_t *)buffer;
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
                return n ? (long)n : -OS_ERROR_AGAIN;
            }
            scheduler_block_on(PIPE_DATA_CHAN(p), 0, &pipe_lock, &f);
            if (scheduler_signal_pending()) {
                spin_unlock_irqrestore(&pipe_lock, f);
                return n ? (long)n : -OS_ERROR_INTR;
            }
            continue;
        }
        destination[n] = p->buffer[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUFFER_SIZE;
        p->count--;
        n++;
        if (p->count == 0) {
            break;
        }
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    if (n > 0) {
        scheduler_wake_all(PIPE_SPACE_CHAN(p));
        scheduler_wake_object(p);
    }
    return (long)n;
}
