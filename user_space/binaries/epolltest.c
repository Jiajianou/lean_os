#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "syscall_wrappers.h"

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
    if (eventfd_read(nb, &v) == 0 || errno != EAGAIN) {
        FAIL(4);
    }
    close(nb);
    return 0;
}

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
    if (read(tf, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) {
        FAIL(6);
    }
    unsigned long waited = (unsigned long)sys_uptime_ms() - started;
    if (expirations != 1 || waited < 55 || waited > 400) {
        FAIL(6);
    }

    it = relative(10, 10);
    if (timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(5);
    }
    usleep(120000);
    if (read(tf, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) {
        FAIL(7);
    }
    if (expirations < 5) {
        FAIL(7);
    }
    struct itimerspec left;
    if (timerfd_gettime(tf, &left) != 0 || left.it_interval.tv_nsec != 10000000L) {
        FAIL(5);
    }
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
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) == 0) {
        FAIL(8);
    }

    struct epoll_event out[8];
    if (epoll_wait(ep, out, 8, 0) != 0) {
        FAIL(9);
    }
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
    if (epoll_wait(ep, out, 4, 0) != 1 || !(out[0].events & EPOLLOUT)) {
        FAIL(10);
    }
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
            FAIL(10);
        }
    }
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
    close(pipefd[0]);
    if (epoll_wait(ep, out, 4, 100) != 1 || !(out[0].events & EPOLLERR)) {
        FAIL(10);
    }
    close(pipefd[1]);
    close(ep);
    return 0;
}

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
    close(pipefd[1]);
    if (epoll_wait(ep, out, 4, 100) != 1 || !(out[0].events & EPOLLHUP)) {
        FAIL(11);
    }
    close(pipefd[0]);
    if (epoll_wait(ep, out, 4, 50) != 0) {
        FAIL(12);
    }

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
    if (epoll_wait(ep, out, 4, 0) != 0) {
        FAIL(13);
    }

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
    if (idled < 10) {
        FAIL(16);
    }
    close(tf);
    close(ep);
    return 0;
}

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
    if (waited < 80 || waited > 2000) {
        FAIL(18);
    }
    close(ev);
    close(ep);
    return rc == 0 ? 0 : 18;
}

static int test_exhaustion(void) {
    int file_descriptors[96];
    int n = 0;
    while (n < 96) {
        int fd = eventfd(0, 0);
        if (fd < 0) {
            break;
        }
        file_descriptors[n++] = fd;
    }
    if (n == 0) {
        FAIL(19);
    }
    for (int i = 0; i < n; i++) {
        close(file_descriptors[i]);
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
        file_descriptors[n++] = fd;
    }
    if (n == 0) {
        FAIL(19);
    }
    for (int i = 0; i < n; i++) {
        close(file_descriptors[i]);
    }
    again = epoll_create1(0);
    if (again < 0) {
        FAIL(19);
    }
    close(again);
    return 0;
}

#define RECORD_BYTES 600
#define RECORDS_PER_WRITER 40

static int write_records(int fd, char letter) {
    char record[RECORD_BYTES];
    memset(record, letter, sizeof(record));
    for (int i = 0; i < RECORDS_PER_WRITER; i++) {
        if (write(fd, record, sizeof(record)) != (ssize_t)sizeof(record)) {
            return 21;
        }
    }
    return 0;
}

static int test_pipe_writes_are_whole(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0 || fcntl(pipefd[1], F_SETFL, O_NONBLOCK) != 0) {
        FAIL(20);
    }
    char bytes[1024];
    memset(bytes, 'p', sizeof(bytes));
    if (write(pipefd[1], bytes, 1000) != 1000) {
        FAIL(20);
    }
    errno = 0;
    if (write(pipefd[1], bytes, 100) != -1 || errno != EAGAIN) {
        FAIL(20);
    }
    char drain[1024];
    if (read(pipefd[0], drain, sizeof(drain)) != 1000) {
        FAIL(20);
    }
    if (write(pipefd[1], bytes, 1024) != 1024 || write(pipefd[1], bytes, 1) != -1) {
        FAIL(20);
    }
    close(pipefd[0]);
    close(pipefd[1]);

    if (pipe(pipefd) != 0) {
        FAIL(21);
    }
    pid_t writers[2];
    for (int w = 0; w < 2; w++) {
        writers[w] = fork();
        if (writers[w] < 0) {
            FAIL(21);
        }
        if (writers[w] == 0) {
            close(pipefd[0]);
            sys_exit(write_records(pipefd[1], w == 0 ? 'A' : 'B'));
        }
    }
    close(pipefd[1]);
    char record[RECORD_BYTES];
    int records = 0;
    int torn = 0;
    for (;;) {
        size_t have = 0;
        while (have < sizeof(record)) {
            ssize_t got = read(pipefd[0], record + have, sizeof(record) - have);
            if (got <= 0) {
                break;
            }
            have += (size_t)got;
        }
        if (have == 0) {
            break;
        }
        if (have != sizeof(record)) {
            torn++;
            break;
        }
        for (size_t i = 1; i < sizeof(record); i++) {
            if (record[i] != record[0]) {
                torn++;
                break;
            }
        }
        records++;
    }
    close(pipefd[0]);
    long first = sys_wait(writers[0]);
    long second = sys_wait(writers[1]);
    if (first != 0 || second != 0) {
        FAIL(21);
    }
    if (torn != 0 || records != 2 * RECORDS_PER_WRITER) {
        printf("epolltest: %d of %d pipe records came out torn\n", torn, records);
        FAIL(22);
    }
    return 0;
}

static int add_watch(int set, int fd, uint32_t events, uint64_t cookie) {
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = events;
    e.data.u64 = cookie;
    return epoll_ctl(set, EPOLL_CTL_ADD, fd, &e);
}

/* M226: a set inside a set, which is how Electron joins libuv's loop to
   Chromium's - libuv's own epoll descriptor goes into a second set, and a
   thread waits on that one to learn when libuv has something to do. */
static int test_epoll_nested(void) {
    int inner = epoll_create1(0);
    int outer = epoll_create1(0);
    int pipefd[2];
    if (inner < 0 || outer < 0 || pipe(pipefd) != 0) {
        FAIL(23);
    }
    if (add_watch(inner, pipefd[0], EPOLLIN | EPOLLONESHOT, 1) != 0 ||
        add_watch(outer, inner, EPOLLIN, 2) != 0) {
        FAIL(23);
    }
    struct epoll_event out[4];
    if (epoll_wait(outer, out, 4, 0) != 0) {
        FAIL(24);
    }
    if (write(pipefd[1], "x", 1) != 1) {
        FAIL(24);
    }
    if (epoll_wait(outer, out, 4, 100) != 1 || out[0].data.u64 != 2 ||
        !(out[0].events & EPOLLIN)) {
        FAIL(24);
    }

    /* Asking the outer set must not spend what the inner set has to report:
       the wait that takes it is libuv's, and it has not happened yet. A
       one-shot watch shows it - asked twice from outside, still armed; taken
       once from inside, and the outer set goes quiet with the byte unread. */
    if (epoll_wait(outer, out, 4, 0) != 1) {
        FAIL(25);
    }
    if (epoll_wait(inner, out, 4, 0) != 1 || out[0].data.u64 != 1) {
        FAIL(25);
    }
    if (epoll_wait(outer, out, 4, 0) != 0) {
        FAIL(25);
    }
    char byte;
    struct epoll_event rearm;
    memset(&rearm, 0, sizeof(rearm));
    rearm.events = EPOLLIN | EPOLLONESHOT;
    rearm.data.u64 = 1;
    if (read(pipefd[0], &byte, 1) != 1 ||
        epoll_ctl(inner, EPOLL_CTL_MOD, pipefd[0], &rearm) != 0) {
        FAIL(25);
    }

    struct pollfd p;
    memset(&p, 0, sizeof(p));
    p.fd = inner;
    p.events = POLLIN;
    if (poll(&p, 1, 0) != 0) {
        FAIL(26);
    }
    if (write(pipefd[1], "y", 1) != 1 || poll(&p, 1, 100) != 1 || !(p.revents & POLLIN)) {
        FAIL(26);
    }
    if (epoll_wait(inner, out, 4, 0) != 1 || read(pipefd[0], &byte, 1) != 1 ||
        epoll_ctl(inner, EPOLL_CTL_MOD, pipefd[0], &rearm) != 0) {
        FAIL(26);
    }

    errno = 0;
    if (add_watch(inner, outer, EPOLLIN, 3) != -1 || errno != ELOOP) {
        FAIL(27);
    }
    errno = 0;
    if (add_watch(outer, outer, EPOLLIN, 4) != -1 || errno != ELOOP) {
        FAIL(27);
    }

    /* Five sets one inside the next is Linux's limit and is allowed; a sixth
       on top is not. */
    int chain[6];
    for (int i = 0; i < 6; i++) {
        chain[i] = epoll_create1(0);
        if (chain[i] < 0) {
            FAIL(28);
        }
    }
    for (int i = 3; i >= 0; i--) {
        if (add_watch(chain[i], chain[i + 1], EPOLLIN, (uint64_t)i) != 0) {
            FAIL(28);
        }
    }
    errno = 0;
    if (add_watch(chain[5], chain[0], EPOLLIN, 5) != -1 || errno != ELOOP) {
        FAIL(28);
    }

    /* A wait on the outer set sleeps until something INSIDE the inner one
       happens - another process's write, and a timer nobody wakes for. */
    pid_t kid = fork();
    if (kid < 0) {
        FAIL(29);
    }
    /* The writer stays alive well past the write: an exit wakes every
       poller on the machine, and a wait that only a broadcast could end
       would pass if the writer left at once. */
    if (kid == 0) {
        usleep(120000);
        if (write(pipefd[1], "z", 1) != 1) {
            sys_exit(29);
        }
        usleep(1500000);
        sys_exit(0);
    }
    unsigned long t0 = (unsigned long)sys_uptime_ms();
    int n = epoll_wait(outer, out, 4, 5000);
    unsigned long write_waited = (unsigned long)sys_uptime_ms() - t0;
    long rc = sys_wait(kid);
    if (rc != 0 || n != 1 || out[0].data.u64 != 2 || write_waited < 80 || write_waited > 1000) {
        FAIL(29);
    }
    unsigned long waited;
    if (epoll_wait(inner, out, 4, 0) != 1 || read(pipefd[0], &byte, 1) != 1) {
        FAIL(29);
    }

    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec it = relative(150, 0);
    if (tf < 0 || add_watch(inner, tf, EPOLLIN, 6) != 0 ||
        timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(30);
    }
    t0 = (unsigned long)sys_uptime_ms();
    n = epoll_wait(outer, out, 4, -1);
    unsigned long timer_waited = (unsigned long)sys_uptime_ms() - t0;
    if (n != 1 || out[0].data.u64 != 2 || timer_waited < 140 || timer_waited > 1000) {
        FAIL(30);
    }
    if (epoll_wait(inner, out, 4, 0) != 1 || out[0].data.u64 != 6) {
        FAIL(30);
    }
    it = relative(150, 0);
    if (timerfd_settime(tf, 0, &it, NULL) != 0) {
        FAIL(31);
    }
    t0 = (unsigned long)sys_uptime_ms();
    if (poll(&p, 1, -1) != 1) {
        FAIL(31);
    }
    waited = (unsigned long)sys_uptime_ms() - t0;
    if (waited < 140 || waited > 1000) {
        FAIL(31);
    }
    printf("epolltest: a set inside a set woke %lu ms after a write 120 ms in, "
           "%lu ms after a 150 ms timer inside it, and poll() %lu ms after one\n",
           write_waited, timer_waited, waited);

    for (int i = 0; i < 6; i++) {
        close(chain[i]);
    }
    close(tf);
    close(pipefd[0]);
    close(pipefd[1]);
    close(outer);
    close(inner);

    /* Each refusal is the errno Linux gives: libuv's loop retries an ADD
       that says EEXIST as a MOD, and stops on anything else. */
    int set = epoll_create1(0);
    int ev = eventfd(0, 0);
    if (set < 0 || ev < 0 || add_watch(set, ev, EPOLLIN, 1) != 0) {
        FAIL(32);
    }
    errno = 0;
    if (add_watch(set, ev, EPOLLIN, 2) != -1 || errno != EEXIST) {
        FAIL(32);
    }
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN;
    errno = 0;
    if (epoll_ctl(set, EPOLL_CTL_MOD, 0, &e) != -1 || errno != ENOENT) {
        FAIL(32);
    }
    errno = 0;
    if (epoll_ctl(set, EPOLL_CTL_DEL, 0, &e) != -1 || errno != ENOENT) {
        FAIL(32);
    }
    close(ev);
    close(set);

    /* EPOLL_CLOEXEC, EFD_CLOEXEC and TFD_CLOEXEC are O_CLOEXEC, as Linux has
       them, and upstream code passes O_CLOEXEC itself - libuv's loop is
       epoll_create1(O_CLOEXEC). Each makes a descriptor that closes on exec;
       a bit none of them knows is EINVAL. */
    int made[3];
    made[0] = epoll_create1(O_CLOEXEC);
    made[1] = eventfd(0, O_CLOEXEC | O_NONBLOCK);
    made[2] = timerfd_create(CLOCK_MONOTONIC, O_CLOEXEC | O_NONBLOCK);
    for (int i = 0; i < 3; i++) {
        if (made[i] < 0 || !(fcntl(made[i], F_GETFD) & FD_CLOEXEC)) {
            FAIL(36);
        }
    }
    eventfd_t nothing;
    errno = 0;
    if (eventfd_read(made[1], &nothing) == 0 || errno != EAGAIN) {
        FAIL(36);
    }
    int plain = epoll_create1(0);
    if (plain < 0 || (fcntl(plain, F_GETFD) & FD_CLOEXEC)) {
        FAIL(36);
    }
    errno = 0;
    if (epoll_create1(O_NONBLOCK) != -1 || errno != EINVAL) {
        FAIL(36);
    }
    errno = 0;
    if (eventfd(0, 0x4000) != -1 || errno != EINVAL) {
        FAIL(36);
    }
    close(plain);
    for (int i = 0; i < 3; i++) {
        close(made[i]);
    }

    /* maxevents is the caller's buffer, and libuv's is 1024 events. */
    static struct epoll_event many[1024];
    int wide = epoll_create1(0);
    int counter = eventfd(1, 0);
    if (wide < 0 || counter < 0 || add_watch(wide, counter, EPOLLIN, 9) != 0 ||
        epoll_wait(wide, many, 1024, 0) != 1 || many[0].data.u64 != 9) {
        FAIL(37);
    }
    close(counter);
    close(wide);
    return 0;
}

/* M226. The machine wakes every poller it has on each TCP tick - every 100
   ms - so a wait that nothing it registered for would end still ends, a
   little late, and a single timing check cannot tell the two apart. Six
   rounds each held to 40 ms after their event can: a correct kernel wakes in
   a few milliseconds, and one that needs the broadcast passes a round about
   two times in five and all six about one time in 250. The event's time is
   taken by the thread that makes it, right before it does. */
#define PROMPT_ROUNDS 6
#define PROMPT_MS 40

typedef struct {
    int target;
    int ack;
    volatile unsigned long written_at;
} prompt_writer_t;

static void *prompt_writer(void *argument) {
    prompt_writer_t *w = (prompt_writer_t *)argument;
    for (int i = 0; i < 2 * PROMPT_ROUNDS; i++) {
        usleep((useconds_t)(37000 + 13000 * (i % 5)));
        w->written_at = (unsigned long)sys_uptime_ms();
        if (eventfd_write(w->target, 1) != 0) {
            return (void *)1;
        }
        eventfd_t taken;
        if (eventfd_read(w->ack, &taken) != 0) {
            return (void *)1;
        }
    }
    return (void *)0;
}

static int test_nested_wakes_are_prompt(void) {
    int inner = epoll_create1(0);
    int outer = epoll_create1(0);
    prompt_writer_t w;
    w.target = eventfd(0, 0);
    w.ack = eventfd(0, 0);
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    if (inner < 0 || outer < 0 || w.target < 0 || w.ack < 0 || tf < 0 ||
        add_watch(inner, w.target, EPOLLIN, 1) != 0 || add_watch(inner, tf, EPOLLIN, 2) != 0 ||
        add_watch(outer, inner, EPOLLIN, 3) != 0) {
        FAIL(33);
    }
    pthread_t thread;
    w.written_at = 0;
    if (pthread_create(&thread, NULL, prompt_writer, &w) != 0) {
        FAIL(33);
    }
    struct epoll_event out[4];
    struct pollfd p;
    memset(&p, 0, sizeof(p));
    p.fd = inner;
    p.events = POLLIN;
    unsigned long worst_wait = 0;
    unsigned long worst_poll = 0;
    for (int i = 0; i < 2 * PROMPT_ROUNDS; i++) {
        int n = i < PROMPT_ROUNDS ? epoll_wait(outer, out, 4, 3000) : poll(&p, 1, 3000);
        unsigned long now = (unsigned long)sys_uptime_ms();
        unsigned long late = now - w.written_at;
        eventfd_t v;
        if (n != 1 || eventfd_read(w.target, &v) != 0 || late > PROMPT_MS) {
            printf("epolltest: round %d woke %lu ms after its write (n=%d)\n", i, late, n);
            FAIL(i < PROMPT_ROUNDS ? 33 : 35);
        }
        if (i < PROMPT_ROUNDS) {
            worst_wait = late > worst_wait ? late : worst_wait;
        } else {
            worst_poll = late > worst_poll ? late : worst_poll;
        }
        if (eventfd_write(w.ack, 1) != 0) {
            FAIL(33);
        }
    }
    void *thread_result;
    if (pthread_join(thread, &thread_result) != 0 || thread_result != (void *)0) {
        FAIL(33);
    }

    unsigned long worst_timer = 0;
    for (int i = 0; i < PROMPT_ROUNDS; i++) {
        long ms = 41 + 17 * i;
        struct itimerspec it = relative(ms, 0);
        unsigned long armed = (unsigned long)sys_uptime_ms();
        if (timerfd_settime(tf, 0, &it, NULL) != 0) {
            FAIL(34);
        }
        int n = epoll_wait(outer, out, 4, 3000);
        unsigned long late = (unsigned long)sys_uptime_ms() - armed - (unsigned long)ms;
        uint64_t expirations;
        if (n != 1 || read(tf, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations) ||
            late > PROMPT_MS) {
            printf("epolltest: timer round %d woke %lu ms late (n=%d)\n", i, late, n);
            FAIL(34);
        }
        worst_timer = late > worst_timer ? late : worst_timer;
    }
    printf("epolltest: a set inside a set, %d rounds each - woken at worst %lu ms after "
           "a write, %lu ms after a timer inside it, and poll() %lu ms after a write\n",
           PROMPT_ROUNDS, worst_wait, worst_timer, worst_poll);
    close(tf);
    close(w.ack);
    close(w.target);
    close(outer);
    close(inner);
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
    if ((rc = test_pipe_writes_are_whole()) != 0) return rc;
    if ((rc = test_epoll_nested()) != 0) return rc;
    if ((rc = test_nested_wakes_are_prompt()) != 0) return rc;
    printf("epolltest: all eleven sections passed\n");
    return 0;
}
