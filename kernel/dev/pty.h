/* kernel/dev/pty.h - M85, second attempt
 *
 * A pair of file descriptors with a terminal in the middle, and the
 * last of M85's six bullets.
 *
 * ---- what a pty is, stated once ---------------------------------------
 *
 * A terminal is a line discipline with hardware on one side and a
 * program on the other. A pty is the same discipline with a *program* on
 * both sides. The master end is where the keyboard would have been: what
 * it writes goes through the discipline exactly as a keystroke does, so
 * ^C raises SIGINT on the foreground job, backspace edits a line, and
 * Enter commits one. The slave end is where the program sits: it reads
 * cooked lines and writes output, and it cannot tell the difference
 * between this and the console - which is the entire point, and the
 * reason a terminal emulator can run a shell without the shell knowing.
 *
 * ---- why this was not in M85's first attempt --------------------------
 *
 * M85 shipped five of six and said so. The missing one was not hard, it
 * was *out of order*: a pty needs a path (`/dev/ptmx`, `/dev/pts/N`),
 * paths need a vnode layer with a mount table, and that was M87 - two
 * milestones later. The terminal object was built first and given a path
 * afterwards, and the pty is the piece that could not be built until the
 * path existed. kernel/dev/tty.h has carried the note since.
 *
 * ---- the two decisions in here ----------------------------------------
 *
 * **A fixed table of eight.** Same reason MAX_TASKS and MAX_FDS are
 * fixed: this kernel has no dynamic table anywhere and one pty per
 * terminal window on a machine with eight window slots is the honest
 * ceiling. A ninth open of /dev/ptmx fails with -1 rather than growing
 * something.
 *
 * **The master is the owner.** Closing the master hangs up the slave -
 * reads on it return end-of-file rather than blocking - and the pair is
 * only recycled when both ends are gone. Doing it the other way round
 * (either end frees the pair) is the bug where a shell that exits takes
 * the terminal emulator's descriptor out from under it.
 */
#pragma once

#include <stdint.h>

#include "tty.h"

/* Eight. See the header note; this is a ceiling, not a design. */
#define PTY_MAX 8

/* Allocates a pair and returns its number, or -1 if all eight are in
 * use. The caller holds the master; the slave is /dev/pts/<n> and is not
 * open until somebody opens it. */
int pty_alloc(void);

/* The terminal in the middle. NULL for a number that names no live pair,
 * which is what makes /dev/pts/3 fail to open rather than open a
 * terminal nothing is driving. */
tty_t *pty_tty(int n);

/* Is `n` a live pair? Cheaper than pty_tty for the callers that only
 * want the question answered - readdir, and the open path. */
int pty_valid(int n);

void pty_slave_opened(int n);
void pty_slave_closed(int n);
void pty_master_closed(int n);

/* ---- the four data operations -----------------------------------------
 *
 * Each one is two lines over tty.c, and they are named from the *end
 * that performs them* rather than from the direction the bytes travel,
 * because that is the thing a reader gets wrong: the master's write is
 * the slave's input, and calling it "write" on both sides is how a pty
 * implementation ends up echoing to itself. */
int64_t pty_master_read(int n, char *buf, uint32_t len);
int64_t pty_master_write(int n, const char *buf, uint32_t len);
int64_t pty_slave_read(int n, char *buf, uint32_t len);
int64_t pty_slave_write(int n, const char *buf, uint32_t len);

/* Would a read on this end return without blocking? 1 yes, 0 no. End of
 * file counts as yes, for the reason fd_is_ready gives about a pipe
 * whose writer has gone: a wait that does not wake on a hangup is a
 * hang. */
int pty_master_readable(int n);
int pty_slave_readable(int n);

/* M85: a session leader has exited - hand back whichever pty (if any) it
 * owned. The console is asked separately by the same caller; this walks
 * the table because a pty's session is not knowable from the task. */
void pty_release_session(int sid);
