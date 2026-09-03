#include "pty.h"

#include "lib/libk.h"
#include "sched/sched.h"

/* One row per pair. `tty` is the discipline; the two open counts are
 * what decides when a pair goes back on the free list, and `allocated`
 * is what makes /dev/pts/<n> refuse a number nobody opened /dev/ptmx
 * for. */
typedef struct {
    tty_t tty;
    uint8_t allocated;
    uint8_t master_open;
    uint8_t slave_open;
    /* Has a slave ever been opened? The master's end-of-file rule needs
     * it: "no slave has this open" is a hangup *after* one did, and is
     * the perfectly ordinary gap between opening /dev/ptmx and opening
     * /dev/pts/<n> before one has. Without this flag a terminal emulator
     * reads end-of-file from a pty it has not finished setting up, which
     * is the classic version of this bug. */
    uint8_t slave_ever_open;
} pty_pair_t;

static pty_pair_t pairs[PTY_MAX];

int pty_alloc(void) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pairs[i].allocated) {
            tty_init_pty(&pairs[i].tty);
            pairs[i].allocated = 1;
            pairs[i].master_open = 1;
            pairs[i].slave_open = 0;
            return i;
        }
    }
    /* Nine terminal windows on a machine with eight window slots. -1
     * rather than a grown table: see pty.h. */
    return -1;
}

int pty_valid(int n) {
    return n >= 0 && n < PTY_MAX && pairs[n].allocated;
}

tty_t *pty_tty(int n) {
    return pty_valid(n) ? &pairs[n].tty : (tty_t *)0;
}

void pty_slave_opened(int n) {
    if (pty_valid(n)) {
        pairs[n].slave_open++;
        pairs[n].slave_ever_open = 1;
    }
}

/* Recycled only when BOTH ends are gone. The pair is the object, and
 * either end alone still names it - which is why this is a count of open
 * ends rather than a flag either end can clear. */
static void maybe_free(int n) {
    if (pairs[n].allocated && !pairs[n].master_open && pairs[n].slave_open == 0) {
        k_memset(&pairs[n], 0, sizeof(pairs[n]));
    }
}

void pty_slave_closed(int n) {
    if (!pty_valid(n)) {
        return;
    }
    if (pairs[n].slave_open > 0) {
        pairs[n].slave_open--;
    }
    if (pairs[n].slave_open == 0 && pairs[n].master_open) {
        /* The last slave has gone, so a master blocked on a read will
         * never get another byte. Waking it is what turns that into an
         * end-of-file rather than a hang. */
        sched_wake_all(SCHED_POLL_CHAN);
    }
    maybe_free(n);
}

void pty_master_closed(int n) {
    if (!pty_valid(n)) {
        return;
    }
    pairs[n].master_open = 0;
    /* The hangup is set on the terminal rather than tracked beside it,
     * because the slave's reader asks the terminal and not this table -
     * it holds a tty_t and nothing else. */
    pairs[n].tty.hup = 1;
    sched_wake_all(SCHED_POLL_CHAN);
    maybe_free(n);
}

void pty_release_session(int sid) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (pairs[i].allocated) {
            (void)tty_release_session(&pairs[i].tty, sid);
        }
    }
}

/* ---- the data path ----------------------------------------------------
 *
 * Every one of these wakes the poll channel after moving bytes, for the
 * same reason pipe_write does: the other end may be parked in
 * SYS_waitfds or in the blocking read inside SYS_read, and a byte
 * delivered to a queue nobody is told about is a byte that arrives when
 * something else happens to wake the reader. */
int64_t pty_master_write(int n, const char *buf, uint32_t len) {
    if (!pty_valid(n)) {
        return -1;
    }
    /* Through the discipline, one byte at a time - which is what makes
     * this a terminal write and not a pipe write. ^C typed into a
     * terminal emulator raises SIGINT here. */
    for (uint32_t i = 0; i < len; i++) {
        tty_input_char(&pairs[n].tty, buf[i]);
    }
    sched_wake_all(SCHED_POLL_CHAN);
    return (int64_t)len;
}

int64_t pty_slave_read(int n, char *buf, uint32_t len) {
    if (!pty_valid(n)) {
        return -1;
    }
    uint32_t got = tty_read(&pairs[n].tty, buf, len);
    if (got == 0 && pairs[n].tty.hup) {
        return 0; /* the master has gone: end of file, not "try again" */
    }
    return (int64_t)got;
}

int64_t pty_slave_write(int n, const char *buf, uint32_t len) {
    if (!pty_valid(n)) {
        return -1;
    }
    tty_write(&pairs[n].tty, buf, len);
    sched_wake_all(SCHED_POLL_CHAN);
    return (int64_t)len;
}

int64_t pty_master_read(int n, char *buf, uint32_t len) {
    if (!pty_valid(n)) {
        return -1;
    }
    return (int64_t)tty_out_read(&pairs[n].tty, buf, len);
}

int pty_master_readable(int n) {
    if (!pty_valid(n)) {
        return 1; /* a dead pair is readable, and reads 0 - never a wait */
    }
    if (tty_out_readable(&pairs[n].tty) > 0) {
        return 1;
    }
    /* Every slave that was open has closed. Nothing can ever be written
     * again, so this is end of file and the master must not park on it. */
    return pairs[n].slave_ever_open && pairs[n].slave_open == 0;
}

int pty_slave_readable(int n) {
    if (!pty_valid(n)) {
        return 1;
    }
    return (tty_readable(&pairs[n].tty) > 0 || pairs[n].tty.hup) ? 1 : 0;
}
