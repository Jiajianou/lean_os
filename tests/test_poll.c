#include "check.h"

#include <string.h>

#define poll lean_poll
#include "../user_space/libc/src/poll.c"
#undef poll

static int readable[16];
static int bad[16];

long sys_waitfds(const int *fds, int count, int timeout_ms) {
    (void)timeout_ms;
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0 && fds[i] < 16 && bad[fds[i]]) {
            return -1;
        }
    }
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0 && fds[i] < 16 && readable[fds[i]]) {
            return i;
        }
    }
    return -2;
}

static void script(int fd, int is_readable) {
    memset(readable, 0, sizeof(readable));
    memset(bad, 0, sizeof(bad));
    readable[fd] = is_readable;
}

TEST(poll, a_readable_socket_asked_only_for_POLLOUT_is_writable) {
    script(5, 1);
    struct pollfd p = {5, POLLOUT, 0};
    CHECK_EQ(lean_poll(&p, 1, 1000), 1);
    CHECK_EQ(p.revents, POLLOUT);
}

TEST(poll, a_readable_socket_asked_for_both_reports_both) {
    script(5, 1);
    struct pollfd p = {5, POLLIN | POLLOUT, 0};
    CHECK_EQ(lean_poll(&p, 1, 1000), 1);
    CHECK_EQ(p.revents, POLLIN | POLLOUT);
}

TEST(poll, a_quiet_socket_asked_for_both_is_writable_and_not_readable) {
    script(5, 0);
    struct pollfd p = {5, POLLIN | POLLOUT, 0};
    CHECK_EQ(lean_poll(&p, 1, 1000), 1);
    CHECK_EQ(p.revents, POLLOUT);
}

TEST(poll, a_readable_socket_asked_for_POLLIN_reports_only_POLLIN) {
    script(5, 1);
    struct pollfd p = {5, POLLIN, 0};
    CHECK_EQ(lean_poll(&p, 1, 1000), 1);
    CHECK_EQ(p.revents, POLLIN);
}

TEST(poll, a_quiet_socket_asked_for_POLLIN_times_out) {
    script(5, 0);
    struct pollfd p = {5, POLLIN, 0};
    CHECK_EQ(lean_poll(&p, 1, 10), 0);
    CHECK_EQ(p.revents, 0);
}

TEST(poll, a_bad_descriptor_is_POLLNVAL) {
    script(5, 0);
    bad[7] = 1;
    struct pollfd p[2] = {{5, POLLOUT, 0}, {7, POLLIN, 0}};
    CHECK_EQ(lean_poll(p, 2, 0), 2);
    CHECK_EQ(p[0].revents, POLLOUT);
    CHECK_EQ(p[1].revents, POLLNVAL);
}
