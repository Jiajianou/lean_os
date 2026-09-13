#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "syscall_wrappers.h"

static volatile int usr1_count;
static volatile int int_count;
static volatile int chld_count;
static volatile int usr2_count;

static void on_usr1(int sig) {
    (void)sig;
    usr1_count++;
}

static void on_int(int sig) {
    (void)sig;
    int_count++;
}

static void on_chld(int sig) {
    (void)sig;
    chld_count++;
}

static void on_usr2(int sig) {
    (void)sig;
    usr2_count++;
}

int main(void) {
    if (signal(SIGUSR1, on_usr1) == SIG_ERR ||
        signal(SIGUSR2, on_usr2) == SIG_ERR ||
        signal(SIGINT, on_int) == SIG_ERR ||
        signal(SIGCHLD, on_chld) == SIG_ERR) {
        return 2;
    }
    if (signal(SIGKILL, on_usr1) != SIG_ERR) {
        return 3;
    }

    volatile long acc = 0;
    for (int i = 1; i <= 100; i++) {
        acc += i;
        if (i == 50) {
            raise(SIGUSR1);
        }
    }
    if (usr1_count != 1) {
        return 4;
    }
    if (acc != 5050) {
        return 5;
    }

    sigset_t usr2 = 1u << SIGUSR2;
    sigprocmask(SIG_BLOCK, &usr2, 0);
    raise(SIGUSR2);
    sys_uptime_ms();
    if (usr2_count != 0) {
        return 7;
    }
    sigprocmask(SIG_UNBLOCK, &usr2, 0);
    sys_uptime_ms();
    if (usr2_count != 1) {
        return 7;
    }

    long child = sys_spawn(PATH_BIN_DIR "hello", "");
    if (child >= 0) {
        long deadline = sys_uptime_ms() + 4000;
        while (chld_count == 0 && sys_uptime_ms() < deadline) {
            sys_yield();
        }
        sys_wait(child);
    }
    if (chld_count == 0) {
        return 6;
    }

    long fd = sys_open(PATH_TMP_DIR "m76ready", OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd >= 0) {
        sys_write((int)fd, "r", 1);
        sys_close((int)fd);
    }

    long deadline = sys_uptime_ms() + 15000;
    while (int_count == 0 && sys_uptime_ms() < deadline) {
        sys_yield();
    }
    if (int_count == 0) {
        return 8;
    }

    fd = sys_open(PATH_TMP_DIR "m76alive", OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd >= 0) {
        char msg[64];
        int n = 0;
        const char *p = "handled-and-alive ";
        for (; *p; p++) {
            msg[n++] = *p;
        }
        msg[n++] = (char)('0' + (int_count % 10));
        sys_write((int)fd, msg, (size_t)n);
        sys_close((int)fd);
    }

    deadline = sys_uptime_ms() + 15000;
    while (usr1_count < 2 && sys_uptime_ms() < deadline) {
        sys_yield();
    }
    return 0;
}
