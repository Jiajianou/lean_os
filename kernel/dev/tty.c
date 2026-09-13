#include "tty.h"

#include "drivers/klog.h"
#include "lib/libk.h"
#include "sched/sched.h"
#include "signal.h"

static tty_t console;

tty_t *tty_console(void) {
    return &console;
}

static void tty_defaults(tty_t *t) {
    t->tio.c_iflag = ICRNL;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_lflag = ISIG | ICANON | ECHO | ECHOE;
    t->tio.c_cc[VINTR] = 3;
    t->tio.c_cc[VQUIT] = 28;
    t->tio.c_cc[VERASE] = 127;
    t->tio.c_cc[VKILL] = 21;
    t->tio.c_cc[VEOF] = 4;
    t->tio.c_cc[VSUSP] = 26;
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
        return;
    }
    t->inbuf[t->in_tail] = c;
    t->in_tail = next;
}

static void commit_line(tty_t *t) {
    for (uint32_t i = 0; i < t->line_len; i++) {
        push(t, t->line[i]);
    }
    t->line_len = 0;
}

static void out_push(tty_t *t, char c) {
    uint32_t next = (t->out_tail + 1) % TTY_OUTBUF;
    if (next == t->out_head) {
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
        echo(t, c);
        push(t, c);
        return;
    }

    if (c == (char)t->tio.c_cc[VERASE] || c == '\b') {
        if (t->line_len > 0) {
            t->line_len--;
            if (t->tio.c_lflag & ECHOE) {
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
        return;
    }
    sched_raise_signal_group(t->fg_pgid, sig);
}

int tty_may_read(tty_t *t, int sid, int pgid) {
    if (!t || t->sid == 0 || t->sid != sid) {
        return 1;
    }
    if (t->fg_pgid == 0 || t->fg_pgid == pgid) {
        return 1;
    }
    sched_raise_signal_group(pgid, SIGTTIN);
    return 0;
}
