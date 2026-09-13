#pragma once

#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

#define PIPE_BUF_SIZE SYS_PIPE_CAPACITY

typedef struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    size_t head, tail, count;
    int read_closed;
    int write_closed;
    int readers, writers;
    int persistent;
} pipe_t;

pipe_t *pipe_create(void);

#define NAMED_PIPE_NAME_LEN 16

pipe_t *pipe_named(const char *name);
void pipe_close_write(pipe_t *p);

void pipe_ref_read(pipe_t *p);
void pipe_ref_write(pipe_t *p);
void pipe_unref_read(pipe_t *p);
void pipe_unref_write(pipe_t *p);

void pipe_reset(pipe_t *p);

int pipe_buffered(pipe_t *p);

int pipe_write_closed(pipe_t *p);

int pipe_read_closed(pipe_t *p);

int pipe_writable(pipe_t *p);

long pipe_write(pipe_t *p, const void *buf, size_t len, int nonblock);

long pipe_read(pipe_t *p, void *buf, size_t maxlen, int nonblock);
