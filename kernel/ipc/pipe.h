/* kernel/ipc/pipe.h
 *
 * M14: a real, blocking, fixed-size circular-buffer pipe. Read and write
 * both cooperatively yield (schedule()) rather than block on a real wait
 * queue - the same "poll + yield" pattern SYS_wait and SYS_read (fd=0)
 * already use, not a new inconsistency introduced here.
 *
 * Ownership was tracked with plain open/closed flags and not a refcount,
 * because this project had no SYS_close to ever need one for.
 *
 * M59 gives it one, and the reason is a specific missing behaviour rather
 * than tidiness: a refcount makes "the last writer went away" knowable,
 * which is the difference between a terminal that sees the end of a
 * command's output and one that waits forever. A command's stdout is an
 * anonymous pipe held by the shell and by the child; nothing could ever
 * tell that the child's end had gone.
 *
 * *Named* pipes deliberately keep the old behaviour, and that is not an
 * oversight - it is the mechanism M55 rests on. A named pipe is a
 * rendezvous point that outlives every process that ever held it, so a
 * client exiting must not make the compositor's read return EOF forever.
 * They are marked `persistent` and their counts are ignored.
 */
#pragma once

#include "syscall.h" /* system_api/include/syscall.h - SYS_PIPE_CAPACITY, the capacity this file and user space have to agree on */

#include <stddef.h>
#include <stdint.h>

/* M56: one definition, shared with user space - see SYS_PIPE_CAPACITY. */
#define PIPE_BUF_SIZE SYS_PIPE_CAPACITY

typedef struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    size_t head, tail, count;
    int read_closed;  /* no reader left - a blocked/future writer gets -1 (broken pipe) instead of blocking forever */
    int write_closed; /* no writer left - a blocked/future reader gets EOF (0) instead of blocking forever */
    /* M59: how many fd-table slots, across every task, name each end. */
    int readers, writers;
    /* M59: a rendezvous point rather than a conversation - see the header
     * comment. Never auto-closes, whatever the counts say. */
    int persistent;
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
/* M52: moved here from pipe.c, where it had been private. syscall.c has
 * to size the kernel-side buffer it copies a SYS_pipe_open name into, and
 * a second hand-picked number that merely happened to be large enough is
 * exactly the near-duplicate cap this project has shipped bugs behind
 * three times (M40, M41, M50). One definition, both users. */
#define NAMED_PIPE_NAME_LEN 16

pipe_t *pipe_named(const char *name);
void pipe_close_read(pipe_t *p);
void pipe_close_write(pipe_t *p);

/* M59: one more / one fewer descriptor naming an end. The unref pair is
 * what finally closes an anonymous pipe when its last holder lets go -
 * and what deliberately does nothing at all for a named one. */
void pipe_ref_read(pipe_t *p);
void pipe_ref_write(pipe_t *p);
void pipe_unref_read(pipe_t *p);
void pipe_unref_write(pipe_t *p);

/* M29: drops every byte currently buffered and clears both closed flags -
 * for a named pipe being handed to a brand-new owner (SYS_pipe_reset) that
 * must not see whatever its predecessor left queued. Nothing else about
 * the pipe_t (its identity/address) changes, so every fd anyone already
 * has open on it keeps working, just against an emptied buffer. */
void pipe_reset(pipe_t *p);

/* M67: how many bytes are buffered right now, read under pipe.c's own
 * lock. SYS_pipe_poll used to reach into `p->count` directly from
 * syscall.c, which was fine while a syscall could not be preempted and
 * is exactly the kind of reach-through that stops being fine when it
 * can. -1 for a NULL pipe. */
int pipe_buffered(pipe_t *p);

/* Blocks while the buffer is full, unless the read end has already
 * closed (returns -1 immediately, or however many bytes got written
 * before that happened). */
long pipe_write(pipe_t *p, const void *buf, size_t len);

/* Blocks until at least one byte is available, then drains whatever's
 * immediately ready (up to maxlen) without blocking further - mirrors
 * SYS_read's fd=0 semantics exactly. Returns 0 (EOF) once the write end
 * has closed and the buffer is empty. */
long pipe_read(pipe_t *p, void *buf, size_t maxlen);
