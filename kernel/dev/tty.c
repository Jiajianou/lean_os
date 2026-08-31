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

void tty_init(void) {
    k_memset(&console, 0, sizeof(console));
    /* The settings a terminal has before anything configures it, which
     * are the ones a person sitting at a keyboard expects: lines, echo,
     * and ^C meaning interrupt. A program that wants otherwise says so. */
    console.tio.c_iflag = ICRNL;
    console.tio.c_oflag = OPOST | ONLCR;
    console.tio.c_lflag = ISIG | ICANON | ECHO | ECHOE;
    console.tio.c_cc[VINTR] = 3;   /* ^C */
    console.tio.c_cc[VQUIT] = 28;  /* ^\ */
    console.tio.c_cc[VERASE] = 127;/* DEL, which is what a backspace key sends */
    console.tio.c_cc[VKILL] = 21;  /* ^U */
    console.tio.c_cc[VEOF] = 4;    /* ^D */
    console.tio.c_cc[VSUSP] = 26;  /* ^Z */
    console.rows = 25;
    console.cols = 80;
    console.fg_pgid = 0;
    console.sid = 0;
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

/* Echo goes to the kernel log, which is where the serial console reads
 * and writes today. That is a placeholder for exactly one milestone: M85
 * builds the discipline, and the question of which hardware is attached
 * to it is what the pty work deferred to a later milestone answers. */
static void echo(tty_t *t, char c) {
    if (!(t->tio.c_lflag & ECHO)) {
        return;
    }
    if (c == '\n' && (t->tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)) {
        klog_putc('\r');
    }
    klog_putc(c);
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
