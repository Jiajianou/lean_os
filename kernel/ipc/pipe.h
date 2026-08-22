/* kernel/ipc/pipe.h
 *
 * M14: a real, blocking, fixed-size circular-buffer pipe. Read and write
 * both cooperatively yield (schedule()) rather than block on a real wait
 * queue - the same "poll + yield" pattern SYS_wait and SYS_read (fd=0)
 * already use, not a new inconsistency introduced here.
 *
 * Ownership is tracked with plain open/closed flags, not a refcount -
 * see fd_close in kernel/arch/x86_64/syscall.c for why that's the right
 * level of simplicity for how many fds can currently point at one pipe.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define PIPE_BUF_SIZE 1024

typedef struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    size_t head, tail, count;
    int read_closed;  /* no reader left - a blocked/future writer gets -1 (broken pipe) instead of blocking forever */
    int write_closed; /* no writer left - a blocked/future reader gets EOF (0) instead of blocking forever */
} pipe_t;

pipe_t *pipe_create(void);
void pipe_close_read(pipe_t *p);
void pipe_close_write(pipe_t *p);

/* Blocks while the buffer is full, unless the read end has already
 * closed (returns -1 immediately, or however many bytes got written
 * before that happened). */
long pipe_write(pipe_t *p, const void *buf, size_t len);

/* Blocks until at least one byte is available, then drains whatever's
 * immediately ready (up to maxlen) without blocking further - mirrors
 * SYS_read's fd=0 semantics exactly. Returns 0 (EOF) once the write end
 * has closed and the buffer is empty. */
long pipe_read(pipe_t *p, void *buf, size_t maxlen);
