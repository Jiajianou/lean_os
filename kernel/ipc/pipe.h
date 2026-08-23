/* kernel/ipc/pipe.h
 *
 * M14: a real, blocking, fixed-size circular-buffer pipe. Read and write
 * both cooperatively yield (schedule()) rather than block on a real wait
 * queue - the same "poll + yield" pattern SYS_wait and SYS_read (fd=0)
 * already use, not a new inconsistency introduced here.
 *
 * Ownership is tracked with plain open/closed flags, not a refcount -
 * this project has no SYS_close (see syscall_wrappers.h's own note) to
 * ever need one for.
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

/* M20: a small find-or-create registry of pipes identified by name
 * instead of inherited fds - the rendezvous mechanism two *unrelated*
 * processes (a compositor and a client the shell launched separately,
 * not parent/child) need, since SYS_pipe's fds only ever reach a child
 * spawned afterward. First call with a given name creates the pipe;
 * every later call with the same name (from any process) returns the
 * exact same one. Fixed small table (8 names) - this project has no
 * dynamic multi-service discovery need beyond M20's own window-creation
 * protocol yet. Returns NULL if the table is full. */
pipe_t *pipe_named(const char *name);
void pipe_close_read(pipe_t *p);
void pipe_close_write(pipe_t *p);

/* M29: drops every byte currently buffered and clears both closed flags -
 * for a named pipe being handed to a brand-new owner (SYS_pipe_reset) that
 * must not see whatever its predecessor left queued. Nothing else about
 * the pipe_t (its identity/address) changes, so every fd anyone already
 * has open on it keeps working, just against an emptied buffer. */
void pipe_reset(pipe_t *p);

/* Blocks while the buffer is full, unless the read end has already
 * closed (returns -1 immediately, or however many bytes got written
 * before that happened). */
long pipe_write(pipe_t *p, const void *buf, size_t len);

/* Blocks until at least one byte is available, then drains whatever's
 * immediately ready (up to maxlen) without blocking further - mirrors
 * SYS_read's fd=0 semantics exactly. Returns 0 (EOF) once the write end
 * has closed and the buffer is empty. */
long pipe_read(pipe_t *p, void *buf, size_t maxlen);
