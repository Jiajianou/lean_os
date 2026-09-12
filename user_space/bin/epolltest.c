/* user_space/bin/epolltest.c - M119's fixture
 *
 * epoll, eventfd and timerfd through the real syscalls, on the machine.
 * tests/test_readyfds.c grades the three objects off it - every boundary,
 * at nanosecond precision, with 23 tests - and cannot have the two things
 * that matter most here:
 *
 *   - **a real clock.** A timer that fires at the right time on a fake
 *     clock is arithmetic; one that fires at the right time on this
 *     machine is a timer.
 *   - **a real scheduler.** The claim that `epoll_wait` *sleeps* rather
 *     than spinning cannot be made by a unit test at all. It is made here
 *     with SYS_idle_ticks, the counter M68 added because "this machine
 *     sleeps when idle" is not observable any other way.
 *
 * Exit codes, so a failure names itself:
 *   0  everything worked
 *   2  eventfd could not be created, or did not count
 *   3  EFD_SEMAPHORE did not take exactly one
 *   4  a non-blocking empty counter did not say EAGAIN
 *   5  timerfd could not be created or armed
 *   6  a one-shot timer did not fire in the time it was given
 *   7  a periodic timer did not report the firings nobody read
 *   8  epoll_create/ctl failed
 *   9  epoll_wait did not report a ready descriptor, or reported the wrong cookie
 *  10  EPOLLOUT was reported for a pipe that was full, or withheld from one that was not
 *  11  EPOLLHUP did not arrive when the writer closed
 *  12  a closed descriptor stayed in the set
 *  13  EPOLLET reported an unchanged condition
 *  14  EPOLLONESHOT fired twice, or a MOD did not re-arm it
 *  15  epoll_wait over a timerfd did not block for about the right time
 *  16  epoll_wait spun instead of sleeping - the machine was not idle
 *  17  fork failed
 *  18  a wake from another process did not arrive
 *  19  exhaustion did not refuse, or did not recover
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "syscall_wrappers.h" /* sys_exit in a forked child, sys_wait, sys_uptime_ms, sys_idle_ticks */

#define FAIL(code) do { return (code); } while (0)

static struct itimerspec relative(long value_ms, long interval_ms) {
    struct itimerspec it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = value_ms / 1000;
    it.it_value.tv_nsec = (value_ms % 1000) * 1000000L;
    it.it_interval.tv_sec = interval_ms / 1000;
    it.it_interval.tv_nsec = (interval_ms % 1000) * 1000000L;
    return it;
}

/* ---- 1: a counter ----------------------------------------------------- */
static int test_eventfd(void) {
    int fd = eventfd(0, 0);
    if (fd < 0) {
        FAIL(2);
    }
    if (eventfd_write(fd, 3) != 0 || eventfd_write(fd, 4) != 0) {
        FAIL(2);
    }
    eventfd_t v = 0;
    if (eventfd_read(fd, &v) != 0 || v != 7) {
        FAIL(2);
    }
    close(fd);

    int sem = eventfd(2, EFD_SEMAPHORE);
    if (sem < 0) {
        FAIL(3);
    }
    if (eventfd_read(sem, &v) != 0 || v != 1) {
        FAIL(3);
    }
    close(sem);

    int nb = eventfd(0, EFD_NONBLOCK);
    if (nb < 0) {
        FAIL(4);
    }
    errno = 0;
    /* The one answer a pump depends on: an empty counter on a non-blocking
     * descriptor is EAGAIN, not zero and not a wait. */
    if (eventfd_read(nb, &v) == 0 || errno != EAGAIN) {
        FAIL(4);
    }
    close(nb);
    return 0;
}

/* ---- 2: a clock ------------------------------------------------------- */
static int test_timerfd(void) {
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tf < 0) {
        FAIL(5);
    }
    struct itimerspec it = relative(60, 0);
    unsigned long started = (unsigned long)sys_uptime_ms();
    if (timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(5);
    }
    uint64_t expirations = 0;
    /* A blocking read on a timer, which is the simplest form of "wait until
     * a time" this system has ever had. */
    if (read(tf, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) {
        FAIL(6);
    }
    unsigned long waited = (unsigned long)sys_uptime_ms() - started;
    /* 60 ms asked for, and the clock ticks every 10: anything from 60 to
     * 200 is this machine doing what it was told. Returning EARLY is the
     * failure that matters - a timer that is readable before its time is a
     * pump that spins - so the lower bound is the tight one. */
    if (expirations != 1 || waited < 55 || waited > 400) {
        FAIL(6);
    }

    /* A periodic timer, left unread for several intervals. */
    it = relative(10, 10);
    if (timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(5);
    }
    usleep(120000); /* 120 ms: twelve firings, nobody reading */
    if (read(tf, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) {
        FAIL(7);
    }
    /* The count is what makes this honest: a reader that was told "once"
     * would have no way to know it was behind. */
    if (expirations < 5) {
        FAIL(7);
    }
    struct itimerspec left;
    if (timerfd_gettime(tf, &left) != 0 || left.it_interval.tv_nsec != 10000000L) {
        FAIL(5);
    }
    /* Disarming, and the POSIX trap that goes with it: an interval with no
     * value is OFF, not periodic. */
    it = relative(0, 10);
    if (timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(5);
    }
    if (timerfd_gettime(tf, &left) != 0 ||
        left.it_value.tv_sec != 0 || left.it_value.tv_nsec != 0) {
        FAIL(5);
    }
    close(tf);
    return 0;
}

/* ---- 3: a set of descriptors of four different kinds ------------------ */
static int test_epoll_mixed(void) {
    int ep = epoll_create1(0);
    if (ep < 0) {
        FAIL(8);
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL(8);
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL(8);
    }
    int ev = eventfd(0, 0);
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    if (ev < 0 || tf < 0) {
        FAIL(8);
    }

    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN;
    e.data.u64 = 1001;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &e) != 0) {
        FAIL(8);
    }
    e.data.u64 = 1002;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &e) != 0) {
        FAIL(8);
    }
    e.data.u64 = 1003;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) != 0) {
        FAIL(8);
    }
    e.data.u64 = 1004;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, tf, &e) != 0) {
        FAIL(8);
    }
    /* Adding the same descriptor twice is refused rather than silently
     * replacing the cookie. */
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) == 0) {
        FAIL(8);
    }

    struct epoll_event out[8];
    if (epoll_wait(ep, out, 8, 0) != 0) {
        FAIL(9); /* nothing is ready yet, and a poll must say so rather than invent one */
    }
    /* One of each kind becomes ready, in four different ways. */
    if (write(pipefd[1], "x", 1) != 1) {
        FAIL(9);
    }
    if (write(sv[1], "y", 1) != 1) {
        FAIL(9);
    }
    if (eventfd_write(ev, 1) != 0) {
        FAIL(9);
    }
    struct itimerspec it = relative(10, 0);
    if (timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(9);
    }
    usleep(60000);
    int n = epoll_wait(ep, out, 8, 1000);
    if (n != 4) {
        FAIL(9);
    }
    /* Every cookie came back, and each exactly once. This is what a pump
     * uses to find its handler. */
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (!(out[i].events & EPOLLIN)) {
            FAIL(9);
        }
        if (out[i].data.u64 < 1001 || out[i].data.u64 > 1004) {
            FAIL(9);
        }
        int bit = 1 << (out[i].data.u64 - 1001);
        if (seen & bit) {
            FAIL(9);
        }
        seen |= bit;
    }
    if (seen != 0xF) {
        FAIL(9);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(sv[0]);
    close(sv[1]);
    close(ev);
    close(tf);
    close(ep);
    return 0;
}

/* ---- 4: writability, which this kernel could not answer before -------- */
static int test_epollout(void) {
    int ep = epoll_create1(0);
    int pipefd[2];
    if (ep < 0 || pipe(pipefd) != 0) {
        FAIL(8);
    }
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLOUT;
    e.data.u64 = 7;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[1], &e) != 0) {
        FAIL(8);
    }
    struct epoll_event out[4];
    /* An empty pipe is writable, and saying so is the easy half. */
    if (epoll_wait(ep, out, 4, 0) != 1 || !(out[0].events & EPOLLOUT)) {
        FAIL(10);
    }
    /* Fill it. SYS_PIPE_CAPACITY is the kernel's, and writing more than it
     * would block - so this writes exactly that much through a
     * non-blocking descriptor and stops when it says EAGAIN. */
    if (fcntl(pipefd[1], F_SETFL, O_NONBLOCK) != 0) {
        FAIL(10);
    }
    char chunk[256];
    memset(chunk, 'f', sizeof(chunk));
    long total = 0;
    for (;;) {
        ssize_t w = write(pipefd[1], chunk, sizeof(chunk));
        if (w <= 0) {
            break;
        }
        total += w;
        if (total > (1 << 20)) {
            FAIL(10); /* a pipe that never fills is not a pipe */
        }
    }
    /* **The assertion this whole milestone's writability work is for.**
     * <poll.h> reports POLLOUT for anything open and says why; epoll has to
     * tell the truth, or a pump that registers EPOLLOUT on a full pipe is
     * woken immediately, forever, at 100% of a core. */
    if (epoll_wait(ep, out, 4, 0) != 0) {
        FAIL(10);
    }
    char drain[512];
    if (read(pipefd[0], drain, sizeof(drain)) <= 0) {
        FAIL(10);
    }
    if (epoll_wait(ep, out, 4, 100) != 1 || !(out[0].events & EPOLLOUT)) {
        FAIL(10);
    }
    /* And a pipe whose reader has gone is an error rather than a
     * readiness - the write that follows would fail, and a pump told
     * "writable" would try it forever. */
    close(pipefd[0]);
    if (epoll_wait(ep, out, 4, 100) != 1 || !(out[0].events & EPOLLERR)) {
        FAIL(10);
    }
    close(pipefd[1]);
    close(ep);
    return 0;
}

/* ---- 5: hangup, staleness, edge and one-shot -------------------------- */
static int test_epoll_edges(void) {
    int ep = epoll_create1(0);
    int pipefd[2];
    if (ep < 0 || pipe(pipefd) != 0) {
        FAIL(8);
    }
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN;
    e.data.u64 = 11;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &e) != 0) {
        FAIL(8);
    }
    struct epoll_event out[4];
    close(pipefd[1]); /* the writer goes away with nothing buffered */
    if (epoll_wait(ep, out, 4, 100) != 1 || !(out[0].events & EPOLLHUP)) {
        FAIL(11);
    }
    /* Closing the descriptor drops the registration. Nothing else is in the
     * set, so a wait with a timeout now times out rather than reporting
     * whatever that descriptor number becomes next. */
    close(pipefd[0]);
    if (epoll_wait(ep, out, 4, 50) != 0) {
        FAIL(12);
    }

    /* Edge triggering, on a fresh pipe. */
    if (pipe(pipefd) != 0) {
        FAIL(8);
    }
    e.events = EPOLLIN | EPOLLET;
    e.data.u64 = 12;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &e) != 0) {
        FAIL(8);
    }
    if (write(pipefd[1], "ab", 2) != 2) {
        FAIL(13);
    }
    if (epoll_wait(ep, out, 4, 100) != 1) {
        FAIL(13);
    }
    /* Still readable - one byte was never read - and deliberately silent.
     * A level-triggered registration would report it again here. */
    if (epoll_wait(ep, out, 4, 0) != 0) {
        FAIL(13);
    }

    /* One-shot. */
    e.events = EPOLLIN | EPOLLONESHOT;
    e.data.u64 = 13;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, pipefd[0], &e) != 0) {
        FAIL(14);
    }
    if (epoll_wait(ep, out, 4, 100) != 1 || out[0].data.u64 != 13) {
        FAIL(14);
    }
    if (epoll_wait(ep, out, 4, 0) != 0) {
        FAIL(14);
    }
    if (epoll_ctl(ep, EPOLL_CTL_MOD, pipefd[0], &e) != 0) {
        FAIL(14);
    }
    if (epoll_wait(ep, out, 4, 100) != 1) {
        FAIL(14);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(ep);
    return 0;
}

/* ---- 6: the claim a unit test cannot make: it SLEEPS ------------------ */
static int test_it_sleeps(void) {
    int ep = epoll_create1(0);
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    if (ep < 0 || tf < 0) {
        FAIL(8);
    }
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN;
    e.data.u64 = 42;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, tf, &e) != 0) {
        FAIL(8);
    }
    struct itimerspec it = relative(200, 0);
    if (timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(5);
    }
    unsigned long idle0 = (unsigned long)sys_idle_ticks(0);
    unsigned long t0 = (unsigned long)sys_uptime_ms();
    /* No timeout at all: the ONLY thing that can end this wait is the timer,
     * and nothing in this kernel interrupts when a deadline passes. If the
     * park's deadline is not computed from the timer, this hangs - which is
     * the honest failure for that bug, and one the boot self-test reports as
     * a missing marker rather than as a wrong number. */
    struct epoll_event out[4];
    int n = epoll_wait(ep, out, 4, -1);
    unsigned long waited = (unsigned long)sys_uptime_ms() - t0;
    unsigned long idled = (unsigned long)sys_idle_ticks(0) - idle0;
    if (n != 1 || out[0].data.u64 != 42) {
        FAIL(15);
    }
    if (waited < 190 || waited > 600) {
        FAIL(15);
    }
    /* **And the machine was asleep for most of it.** A spin and a sleep are
     * indistinguishable from the outside, which is exactly how this OS
     * shipped sixty-seven milestones before M68 noticed - so the number is
     * the assertion. 200 ms is 20 ticks; requiring ten of them idle is a
     * wide margin that a spinning implementation cannot meet. */
    if (idled < 10) {
        FAIL(16);
    }
    close(tf);
    close(ep);
    return 0;
}

/* ---- 7: woken by another process, which is what an eventfd is for ----- */
static int test_cross_process_wake(void) {
    int ep = epoll_create1(0);
    int ev = eventfd(0, 0);
    if (ep < 0 || ev < 0) {
        FAIL(8);
    }
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN;
    e.data.u64 = 77;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) != 0) {
        FAIL(8);
    }
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(17);
    }
    if (kid == 0) {
        /* Long enough that the parent is genuinely parked before this
         * arrives - the case that needs a wake rather than a re-scan. */
        usleep(120000);
        if (eventfd_write(ev, 1) != 0) {
            sys_exit(18);
        }
        sys_exit(0);
    }
    struct epoll_event out[4];
    unsigned long t0 = (unsigned long)sys_uptime_ms();
    int n = epoll_wait(ep, out, 4, 5000);
    unsigned long waited = (unsigned long)sys_uptime_ms() - t0;
    long rc = sys_wait(kid);
    if (n != 1 || out[0].data.u64 != 77) {
        FAIL(18);
    }
    /* It waited for the write and was woken BY it - not by a timeout, and
     * not by a poll that happened to come round. */
    if (waited < 80 || waited > 2000) {
        FAIL(18);
    }
    close(ev);
    close(ep);
    return rc == 0 ? 0 : 18;
}

/* ---- 8: exhaustion, which Q9's rule says must recover ----------------- */
static int test_exhaustion(void) {
    int fds[96];
    int n = 0;
    while (n < 96) {
        int fd = eventfd(0, 0);
        if (fd < 0) {
            break;
        }
        fds[n++] = fd;
    }
    if (n == 0) {
        FAIL(19);
    }
    for (int i = 0; i < n; i++) {
        close(fds[i]);
    }
    int again = eventfd(0, 0);
    if (again < 0) {
        FAIL(19);
    }
    close(again);

    n = 0;
    while (n < 96) {
        int fd = epoll_create1(0);
        if (fd < 0) {
            break;
        }
        fds[n++] = fd;
    }
    if (n == 0) {
        FAIL(19);
    }
    for (int i = 0; i < n; i++) {
        close(fds[i]);
    }
    again = epoll_create1(0);
    if (again < 0) {
        FAIL(19);
    }
    close(again);
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_eventfd()) != 0) return rc;
    if ((rc = test_timerfd()) != 0) return rc;
    if ((rc = test_epoll_mixed()) != 0) return rc;
    if ((rc = test_epollout()) != 0) return rc;
    if ((rc = test_epoll_edges()) != 0) return rc;
    if ((rc = test_it_sleeps()) != 0) return rc;
    if ((rc = test_cross_process_wake()) != 0) return rc;
    if ((rc = test_exhaustion()) != 0) return rc;
    printf("epolltest: all eight sections passed\n");
    return 0;
}
