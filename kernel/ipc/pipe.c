#include "pipe.h"

#include "lib/libk.h"
#include "mm/heap.h"
#include "sched/sched.h"

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
    return p;
}

void pipe_close_read(pipe_t *p) {
    p->read_closed = 1;
}

void pipe_close_write(pipe_t *p) {
    p->write_closed = 1;
}

#define MAX_NAMED_PIPES     24 /* M21: bumped from 8 - wm_req/wm_resp plus one event pipe per connected window (system_api/include/wm.h's wm_event_pipe_name) can now outgrow the old cap. */
#define NAMED_PIPE_NAME_LEN 16

typedef struct {
    char name[NAMED_PIPE_NAME_LEN];
    pipe_t *p;
} named_pipe_t;

static named_pipe_t named_pipes[MAX_NAMED_PIPES];
static int named_pipe_count;

pipe_t *pipe_named(const char *name) {
    for (int i = 0; i < named_pipe_count; i++) {
        if (k_strcmp(named_pipes[i].name, name) == 0) {
            return named_pipes[i].p;
        }
    }
    if (named_pipe_count >= MAX_NAMED_PIPES) {
        return (pipe_t *)0;
    }
    pipe_t *p = pipe_create();
    if (!p) {
        return (pipe_t *)0;
    }
    k_strlcpy(named_pipes[named_pipe_count].name, name, NAMED_PIPE_NAME_LEN);
    named_pipes[named_pipe_count].p = p;
    named_pipe_count++;
    return p;
}

long pipe_write(pipe_t *p, const void *buf, size_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;
    while (written < len) {
        if (p->read_closed) {
            return written > 0 ? (long)written : -1;
        }
        if (p->count == PIPE_BUF_SIZE) {
            schedule();
            continue;
        }
        p->buf[p->head] = src[written];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
        written++;
    }
    return (long)written;
}

long pipe_read(pipe_t *p, void *buf, size_t maxlen) {
    uint8_t *dst = (uint8_t *)buf;
    size_t n = 0;
    while (n < maxlen) {
        if (p->count == 0) {
            if (p->write_closed) {
                return (long)n;
            }
            schedule();
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
    return (long)n;
}
