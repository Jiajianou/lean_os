#include "pty.h"

#include "library/kernel_library.h"
#include "scheduler/scheduler.h"

typedef struct {
    tty_t tty;
    uint8_t allocated;
    uint8_t master_open;
    uint8_t slave_open;
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
        scheduler_wake_all(SCHEDULER_POLL_CHAN);
    }
    maybe_free(n);
}

void pty_master_closed(int n) {
    if (!pty_valid(n)) {
        return;
    }
    pairs[n].master_open = 0;
    pairs[n].tty.hup = 1;
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    maybe_free(n);
}

void pty_release_session(int sid) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (pairs[i].allocated) {
            (void)tty_release_session(&pairs[i].tty, sid);
        }
    }
}

int64_t pty_master_write(int n, const char *buffer, uint32_t length) {
    if (!pty_valid(n)) {
        return -1;
    }
    for (uint32_t i = 0; i < length; i++) {
        tty_input_char(&pairs[n].tty, buffer[i]);
    }
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    return (int64_t)length;
}

int64_t pty_slave_read(int n, char *buffer, uint32_t length) {
    if (!pty_valid(n)) {
        return -1;
    }
    uint32_t got = tty_read(&pairs[n].tty, buffer, length);
    if (got == 0 && pairs[n].tty.hup) {
        return 0;
    }
    return (int64_t)got;
}

int64_t pty_slave_write(int n, const char *buffer, uint32_t length) {
    if (!pty_valid(n)) {
        return -1;
    }
    tty_write(&pairs[n].tty, buffer, length);
    scheduler_wake_all(SCHEDULER_POLL_CHAN);
    return (int64_t)length;
}

int64_t pty_master_read(int n, char *buffer, uint32_t length) {
    if (!pty_valid(n)) {
        return -1;
    }
    return (int64_t)tty_out_read(&pairs[n].tty, buffer, length);
}

int pty_master_readable(int n) {
    if (!pty_valid(n)) {
        return 1;
    }
    if (tty_out_readable(&pairs[n].tty) > 0) {
        return 1;
    }
    return pairs[n].slave_ever_open && pairs[n].slave_open == 0;
}

int pty_slave_readable(int n) {
    if (!pty_valid(n)) {
        return 1;
    }
    return (tty_readable(&pairs[n].tty) > 0 || pairs[n].tty.hup) ? 1 : 0;
}
