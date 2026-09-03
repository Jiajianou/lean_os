/* kernel/dev/tty.h - M85
 *
 * A terminal, as an object with rules, rather than as "whatever is on the
 * other end of fd 0".
 *
 * <unistd.h>'s isatty has said since M77 that "there is no terminal
 * device here to ask, so this is the honest approximation and not a
 * stub". This is the thing to ask. What makes it a terminal rather than
 * another pipe is three properties a pipe does not have:
 *
 *   - a LINE DISCIPLINE. Bytes arriving from a keyboard are not what a
 *     program reads. In canonical mode they are collected into a line
 *     that backspace can edit and that is handed over whole when Enter
 *     is pressed, which is why every program on a Unix gets line editing
 *     without containing any.
 *   - a FOREGROUND PROCESS GROUP. A terminal is talking to one job at a
 *     time, and ^C interrupts *that* job - all of its stages - rather
 *     than whichever process happens to be reading.
 *   - the ability to STOP a program. ^Z is not a character a program
 *     reads; it is a signal the terminal raises, and the program it
 *     suspends never sees it at all.
 *
 * What this deliberately is NOT, yet: a file. `/dev/tty` needs device
 * files, device files need a vnode layer with a mount table, and that is
 * M87. The arc as written put M85 before M87, which turns out to have
 * been the wrong order for this one bullet - so the terminal is a kernel
 * object reachable through syscalls here, and M87 is where it also
 * becomes a path. Nothing about the discipline changes when it does.
 */
#pragma once

#include <stdint.h>

#include "termios.h" /* system_api/include/termios.h - the shared struct */

/* One line being edited, and the bytes waiting to be read.
 *
 * The line buffer is separate from the read buffer on purpose: in
 * canonical mode a line that is still being edited is not readable, and
 * a single buffer would make a half-typed line visible to a program that
 * happened to read at the wrong moment. Enter is what moves bytes from
 * one to the other, and that move is the whole of "canonical". */
#define TTY_LINE_MAX 512
#define TTY_INBUF    2048

/* M85 (second attempt): the other direction, which the console never
 * needed and a pty cannot work without.
 *
 * The console's output goes to klog because that is where the hardware
 * is. A pty has no hardware: the thing on the other side of it is a
 * *program*, and everything the slave writes - plus everything the
 * discipline echoes - has to be queued somewhere that program can read.
 * So a terminal grows an output queue, and which of the two sinks it
 * uses is the only difference between the console and a pty. */
#define TTY_OUTBUF   4096

typedef struct tty {
    char line[TTY_LINE_MAX];
    uint32_t line_len;

    char inbuf[TTY_INBUF];
    uint32_t in_head; /* next byte a reader takes */
    uint32_t in_tail; /* next byte the discipline writes */

    struct termios tio;

    /* Which job this terminal is talking to, and which session owns it.
     * A terminal with no session (sid 0) belongs to nobody and signals
     * nothing - which is what a terminal looks like before a shell has
     * claimed it. */
    int fg_pgid;
    int sid;

    uint16_t rows, cols;

    /* Where this terminal's output goes. 0 is the console - klog, and the
     * serial line and framebuffer behind it. 1 is a pty, and the bytes go
     * into outbuf for whoever holds the master to read. */
    uint8_t is_pty;

    /* M85: the master has gone.
     *
     * A slave whose master has closed can never receive another byte, so
     * a read on it must report end-of-file rather than block forever.
     * This is the same line pipe_write_closed draws for a pipe and it is
     * load-bearing for the same reason: without it every program holding
     * the slave end of a closed pty hangs instead of exiting. */
    uint8_t hup;

    char outbuf[TTY_OUTBUF];
    uint32_t out_head; /* next byte the master takes */
    uint32_t out_tail; /* next byte the discipline or the slave writes */
} tty_t;

/* The console terminal - the one attached to this machine's own
 * hardware. Every other terminal here is a pty, created by opening
 * /dev/ptmx; see kernel/dev/pty.h. */
tty_t *tty_console(void);

/* M85: bring one up as a pty rather than as the console. Same discipline,
 * different sink - which is the whole of what "pty" means. */
void tty_init_pty(tty_t *t);

void tty_init(void);

/* Feed one byte in from whatever hardware is attached. This is where the
 * discipline lives: signals are raised, characters are echoed, the line
 * is edited, and Enter commits. Returns the number of bytes echoed, which
 * the caller writes wherever its output goes. */
void tty_input_char(tty_t *t, char c);

/* How many bytes a reader could take right now. In canonical mode this
 * is zero until a whole line has been committed, which is what makes a
 * program block until Enter rather than until a keypress. */
uint32_t tty_readable(const tty_t *t);

/* Takes up to `len` bytes. Returns the count, or 0 if nothing is ready.
 * Never blocks - the blocking belongs to the syscall layer, which is
 * where every other descriptor in this kernel blocks. */
uint32_t tty_read(tty_t *t, char *buf, uint32_t len);

/* M85: may `pgid`, in session `sid`, read this terminal?
 *
 * Returns 1 for yes. Returns 0 having raised SIGTTIN on the caller's
 * group for "no" - which is the answer, not an error: a background job
 * that reads its terminal is stopped, and if it is ever resumed in the
 * foreground the read simply happens. That is why the shell can say
 * "[1]+ Stopped" and `fg` works. */
int tty_may_read(tty_t *t, int sid, int pgid);

/* Raise `sig` on whichever job this terminal is currently talking to. A
 * terminal nobody has claimed signals nobody, which is the state before a
 * shell calls tcsetpgrp. */
void tty_signal_foreground(tty_t *t, int sig);

/* ---- M85: the output side --------------------------------------------
 *
 * tty_write is what the *slave* end does with the bytes a program prints:
 * output processing (ONLCR) and then the sink. tty_out_read is what the
 * *master* end takes back out. On the console the sink is klog and
 * tty_out_read never returns anything, because there is nothing on the
 * other side of a console to read it. */
/* M85 (second attempt): the session that owned this terminal has ended.
 *
 * POSIX: when a session leader exits, SIGHUP goes to the foreground
 * process group and the terminal is disassociated. Both halves matter
 * and the second is the one this machine noticed first - a terminal
 * still owned by a dead session cannot be claimed by anything, so the
 * second program to run in a pty is refused its own terminal by a
 * process that no longer exists. Returns 1 if this terminal was the
 * session's. */
int tty_release_session(tty_t *t, int sid);

void tty_write(tty_t *t, const char *buf, uint32_t len);
uint32_t tty_out_readable(const tty_t *t);
uint32_t tty_out_read(tty_t *t, char *buf, uint32_t len);
