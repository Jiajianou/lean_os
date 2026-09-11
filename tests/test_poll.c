/* tests/test_poll.c - M116
 *
 * poll(), against a scripted SYS_waitfds.
 *
 * poll() here is built out of SYS_waitfds, which answers one question -
 * is this descriptor readable - and <poll.h> says writability is always
 * true on this machine, because nothing in the kernel can say otherwise.
 * The function had that right for a descriptor that was NOT readable and
 * wrong for one that was: a readable descriptor reported only the POLLIN
 * half of what it was asked, so a socket asked for POLLOUT alone while
 * data was already waiting on it reported nothing at all.
 *
 * That is exactly how libcurl waits for a non-blocking connect: POLLOUT,
 * then SO_ERROR. A server that speaks first - which the M116 input test's
 * fixture does, and so do SMTP, FTP and SSH - puts data on the socket
 * before curl looks, and curl then waited for a connect that had already
 * finished, never sent its request, never read, and the browser sat on
 * "Loading" with the window closed at zero. Found by
 * browser_loads_a_page_from_another_machine, which passed on its first run
 * and failed on every one after - a race between the reply and the poll.
 *
 * poll.c is #included with its one function renamed, the way test_malloc
 * includes malloc.c, so the code under test is the code the machine runs. */
#include "check.h"

#include <string.h>

#define poll lean_poll
#include "../user_space/libc/src/poll.c"
#undef poll

/* The kernel, as far as poll.c can see it: which descriptors are
 * readable. -1 for "no such descriptor". */
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
    return -2; /* the deadline passed - timeout 0 here means "right now" */
}

static void script(int fd, int is_readable) {
    memset(readable, 0, sizeof(readable));
    memset(bad, 0, sizeof(bad));
    readable[fd] = is_readable;
}

/* The case that hung the browser: data already waiting, and the caller
 * asking only whether it may write. */
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
    /* The first pass is skipped for a set that asks for POLLOUT, so the
     * second asks each descriptor on its own and finds the bad one. */
    CHECK_EQ(lean_poll(p, 2, 0), 2);
    CHECK_EQ(p[0].revents, POLLOUT);
    CHECK_EQ(p[1].revents, POLLNVAL);
}
