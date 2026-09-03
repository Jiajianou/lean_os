#include "tty.h"

#include "drivers/klog.h"
#include "lib/libk.h"
#include "sched/sched.h"
#include "signal.h" /* system_api/include/signal.h */

/* The one terminal. See tty.h for why one rather than a table. */
static tty_t console;

tty_t *tty_console(void) {
    return &console;
}

/* The settings every terminal starts with, console or pty alike. Shared
 * rather than duplicated because a pty whose defaults differed from the
 * console's would be a second terminal with a second set of rules, and
 * "the same discipline, a different sink" is the entire claim. */
static void tty_defaults(tty_t *t) {
    /* The settings a terminal has before anything configures it, which
     * are the ones a person sitting at a keyboard expects: lines, echo,
     * and ^C meaning interrupt. A program that wants otherwise says so. */
    t->tio.c_iflag = ICRNL;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_lflag = ISIG | ICANON | ECHO | ECHOE;
    t->tio.c_cc[VINTR] = 3;   /* ^C */
    t->tio.c_cc[VQUIT] = 28;  /* ^\ */
    t->tio.c_cc[VERASE] = 127;/* DEL, which is what a backspace key sends */
    t->tio.c_cc[VKILL] = 21;  /* ^U */
    t->tio.c_cc[VEOF] = 4;    /* ^D */
    t->tio.c_cc[VSUSP] = 26;  /* ^Z */
    t->rows = 25;
    t->cols = 80;
    t->fg_pgid = 0;
    t->sid = 0;
}

void tty_init(void) {
    k_memset(&console, 0, sizeof(console));
    tty_defaults(&console);
}

void tty_init_pty(tty_t *t) {
    k_memset(t, 0, sizeof(*t));
    tty_defaults(t);
    t->is_pty = 1;
}

static void push(tty_t *t, char c) {
    uint32_t next = (t->in_tail + 1) % TTY_INBUF;
    if (next == t->in_head) {
        /* Full. The byte is dropped rather than overwriting the oldest,
         * because a terminal buffer that discards what a program has not
         * read yet loses the beginning of a command rather than the end
         * of one - and the beginning is the part that matters. */
        return;
    }
    t->inbuf[t->in_tail] = c;
    t->in_tail = next;
}

/* Commits the line being edited into the read buffer. This is the whole
 * of "canonical": until this runs, nothing a program can read exists. */
static void commit_line(tty_t *t) {
    for (uint32_t i = 0; i < t->line_len; i++) {
        push(t, t->line[i]);
    }
    t->line_len = 0;
}

/* ---- where this terminal's output goes --------------------------------
 *
 * The console's sink is klog, which is the serial line and the
 * framebuffer behind it. A pty's sink is a queue, because the thing on
 * the other side of a pty is a program rather than hardware and it reads
 * those bytes with a read() like any other.
 *
 * Everything above this line - the discipline, the echo, the signals - is
 * identical for both. That is what makes a pty a pty rather than a second
 * terminal implementation. */
static void out_push(tty_t *t, char c) {
    uint32_t next = (t->out_tail + 1) % TTY_OUTBUF;
    if (next == t->out_head) {
        /* Full: the master is not reading. Dropped rather than blocking,
         * for the reason the input side gives - a terminal that blocks
         * its writer because nobody is looking at the screen stops the
         * program instead of the output. Real ptys drop here too. */
        return;
    }
    t->outbuf[t->out_tail] = c;
    t->out_tail = next;
}

static void sink(tty_t *t, char c) {
    if (t->is_pty) {
        out_push(t, c);
    } else {
        klog_putc(c);
    }
}

static void echo(tty_t *t, char c) {
    if (!(t->tio.c_lflag & ECHO)) {
        return;
    }
    if (c == '\n' && (t->tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
        sink(t, '\r');
    }
    sink(t, c);
}

int tty_release_session(tty_t *t, int sid) {
    if (!t || sid == 0 || t->sid != sid) {
        return 0;
    }
    /* SIGHUP first, while fg_pgid still says who to send it to. A
     * program in a terminal whose session leader died is a program whose
     * input can never arrive again, and SIGHUP is how it is told - which
     * is what stops a shell's children outliving the shell as orphans
     * nobody can reach. */
    if (t->fg_pgid != 0) {
        sched_raise_signal_group(t->fg_pgid, SIGHUP);
    }
    t->sid = 0;
    t->fg_pgid = 0;
    return 1;
}

void tty_write(tty_t *t, const char *buf, uint32_t len) {
    if (!t || !buf) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        /* ONLCR is output processing rather than a courtesy: a program
         * that writes "\n" on a terminal in cooked mode gets a carriage
         * return with it, and one that turns OPOST off gets exactly the
         * bytes it wrote. Doing it here rather than in the caller is what
         * makes `stty -opost` mean something. */
        if (buf[i] == '\n' &&
            (t->tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
            sink(t, '\r');
        }
        sink(t, buf[i]);
    }
}

uint32_t tty_out_readable(const tty_t *t) {
    if (!t) {
        return 0;
    }
    return (t->out_tail + TTY_OUTBUF - t->out_head) % TTY_OUTBUF;
}

uint32_t tty_out_read(tty_t *t, char *buf, uint32_t len) {
    uint32_t n = 0;
    if (!t || !buf) {
        return 0;
    }
    while (n < len && t->out_head != t->out_tail) {
        buf[n++] = t->outbuf[t->out_head];
        t->out_head = (t->out_head + 1) % TTY_OUTBUF;
    }
    return n;
}

void tty_input_char(tty_t *t, char c) {
    if (!t) {
        return;
    }
    if ((t->tio.c_iflag & ICRNL) && c == '\r') {
        c = '\n';
    }

    /* ---- signals, before anything else -------------------------------
     *
     * These characters are not input. A program in the foreground never
     * sees a ^C; what it gets is a SIGINT, and the difference is the
     * reason ^C can interrupt a program that is not reading at all.
     *
     * Raised on the foreground process GROUP, which is what makes ^C
     * stop a whole pipeline rather than whichever stage happened to be
     * holding the terminal. */
    if (t->tio.c_lflag & ISIG) {
        if (c == (char)t->tio.c_cc[VINTR]) {
            tty_signal_foreground(t, SIGINT);
            return;
        }
        if (c == (char)t->tio.c_cc[VQUIT]) {
            tty_signal_foreground(t, SIGQUIT);
            return;
        }
        if (c == (char)t->tio.c_cc[VSUSP]) {
            tty_signal_foreground(t, SIGTSTP);
            return;
        }
    }

    if (!(t->tio.c_lflag & ICANON)) {
        /* Raw: every byte is readable the moment it arrives, which is
         * what an editor asks for when it turns ICANON off. */
        echo(t, c);
        push(t, c);
        return;
    }

    if (c == (char)t->tio.c_cc[VERASE] || c == '\b') {
        if (t->line_len > 0) {
            t->line_len--;
            if (t->tio.c_lflag & ECHOE) {
                /* Backspace, space, backspace - the sequence that makes a
                 * character actually disappear from a display that has no
                 * other way to unprint one. */
                echo(t, '\b');
                echo(t, ' ');
                echo(t, '\b');
            }
        }
        return;
    }
    if (c == (char)t->tio.c_cc[VKILL]) {
        while (t->line_len > 0) {
            t->line_len--;
            if (t->tio.c_lflag & ECHOE) {
                echo(t, '\b');
                echo(t, ' ');
                echo(t, '\b');
            }
        }
        return;
    }
    if (c == (char)t->tio.c_cc[VEOF]) {
        /* End of input: whatever has been typed is committed, and an
         * empty line commits nothing - which is what makes ^D on an empty
         * line read as zero bytes and therefore as end-of-file. */
        commit_line(t);
        return;
    }

    if (c == '\n') {
        t->line[t->line_len < TTY_LINE_MAX ? t->line_len : TTY_LINE_MAX - 1] = '\n';
        if (t->line_len < TTY_LINE_MAX) {
            t->line_len++;
        }
        echo(t, '\n');
        commit_line(t);
        return;
    }

    if (t->line_len < TTY_LINE_MAX - 1) {
        t->line[t->line_len++] = c;
        echo(t, c);
    }
    /* A line longer than the buffer simply stops accepting characters.
     * Not an error and not a truncation of what was already typed: the
     * person can see nothing is appearing, which is the clearest possible
     * report from a device whose only output is what it echoes. */
}

uint32_t tty_readable(const tty_t *t) {
    if (!t) {
        return 0;
    }
    return (t->in_tail + TTY_INBUF - t->in_head) % TTY_INBUF;
}

uint32_t tty_read(tty_t *t, char *buf, uint32_t len) {
    uint32_t n = 0;
    while (n < len && t->in_head != t->in_tail) {
        buf[n++] = t->inbuf[t->in_head];
        t->in_head = (t->in_head + 1) % TTY_INBUF;
    }
    return n;
}

void tty_signal_foreground(tty_t *t, int sig) {
    if (!t || t->fg_pgid == 0) {
        /* A terminal nobody has claimed signals nobody. This is the state
         * before a shell calls tcsetpgrp, and dropping the signal is
         * right: there is no job to interrupt. */
        return;
    }
    sched_raise_signal_group(t->fg_pgid, sig);
}

int tty_may_read(tty_t *t, int sid, int pgid) {
    if (!t || t->sid == 0 || t->sid != sid) {
        /* Not this process's controlling terminal. Reading it is allowed
         * - the restriction below is about jobs within one session, not
         * about keeping other sessions out, which is what file
         * permissions would be for if this machine had them. */
        return 1;
    }
    if (t->fg_pgid == 0 || t->fg_pgid == pgid) {
        return 1;
    }
    /* A background job read its terminal. Stopping it is the answer
     * rather than an error: if it is later brought to the foreground the
     * read simply happens, which is what makes `fg` work. */
    sched_raise_signal_group(pgid, SIGTTIN);
    return 0;
}
