#include <errno.h>
#include <fcntl.h>
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
    printf("epolltest: all nine sections passed\n");
    return 0;
}
