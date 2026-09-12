#include "check.h"

#include <string.h>

/* M117: the two calls every wmclient main loop now ends with. wmclient.c
 * is compiled in whole against scripted syscalls, the way test_poll.c
 * compiles poll.c: what is graded is the contract between a client and
 * the kernel's SYS_waitfds - which descriptors are waited on, in what
 * order, for how long - and the one 16-byte message wm_present sends,
 * because a present that named the wrong window or the wrong action
 * would render as "the clock is a second late", not as a failure. */
/* test_poll.c owns the global sys_waitfds fake and the fakes directory
 * owns sys_yield, sys_close, sys_uptime_ms and sys_getpid; this file
 * needs to see what wmclient passes to the wait, so the call is renamed
 * on the way in. */
#define sys_waitfds wm_test_waitfds
#include "../user_space/lib/wmclient.c"
#undef sys_waitfds

/* ---- the scripted kernel ---------------------------------------------- */

static int last_fds[16];
static int last_count;
static int last_timeout;
static long waitfds_answer;

long wm_test_waitfds(const int *fds, int count, int timeout_ms) {
    last_count = count;
    last_timeout = timeout_ms;
    for (int i = 0; i < count && i < 16; i++) {
        last_fds[i] = fds[i];
    }
    return waitfds_answer;
}

static unsigned char written[256];
static size_t written_len;
static int written_fd;

long sys_write(int fd, const void *buf, size_t len) {
    written_fd = fd;
    written_len = len < sizeof(written) ? len : sizeof(written);
    memcpy(written, buf, written_len);
    return (long)len;
}

static int pipe_open_calls;

long sys_pipe_open(const char *name, int fds_out[2]) {
    (void)name;
    pipe_open_calls++;
    fds_out[0] = 40;
    fds_out[1] = 41;
    return 0;
}

/* Everything else wmclient.c can reach, none of it on the paths under
 * test - here so the file links, and answering "nothing happened". */
long sys_read(int fd, void *buf, size_t len) { (void)fd; (void)buf; (void)len; return -1; }
long sys_pipe_poll(int fd) { (void)fd; return 0; }
long sys_task_alive(long pid) { (void)pid; return 1; }
long sys_shm_map(long id) { (void)id; return -1; }
long sys_shm_unmap(void *vaddr, unsigned long bytes) { (void)vaddr; (void)bytes; return 0; }

static wm_window_t window(int evt_fd, int32_t id) {
    wm_window_t w;
    memset(&w, 0, sizeof(w));
    w.evt_fd = evt_fd;
    w.window_id = id;
    w.compositor_pid = -1;
    return w;
}

static void reset(void) {
    memset(last_fds, 0, sizeof(last_fds));
    last_count = -1;
    last_timeout = -99;
    waitfds_answer = -2;
    written_len = 0;
    written_fd = -1;
    pipe_open_calls = 0;
    action_fds[0] = -1;
    action_fds[1] = -1;
}

/* ---- wm_wait_ms --------------------------------------------------------- */

TEST(wmclient, the_event_pipe_is_waited_on_for_exactly_the_time_asked) {
    reset();
    wm_window_t w = window(12, 3);
    CHECK_EQ(wm_wait_ms(&w, NULL, 0, 30), 0);
    CHECK_EQ(last_count, 1);
    CHECK_EQ(last_fds[0], 12);
    CHECK_EQ(last_timeout, 30);
}

TEST(wmclient, no_deadline_and_long_deadlines_are_capped_at_the_liveness_period) {
    reset();
    wm_window_t w = window(12, 3);
    wm_wait_ms(&w, NULL, 0, -1);
    CHECK_EQ(last_timeout, WM_WAIT_CAP_MS);
    wm_wait_ms(&w, NULL, 0, 10000);
    CHECK_EQ(last_timeout, WM_WAIT_CAP_MS);
    /* Exactly the cap is not shortened - a caller may ask for it. */
    wm_wait_ms(&w, NULL, 0, WM_WAIT_CAP_MS);
    CHECK_EQ(last_timeout, WM_WAIT_CAP_MS);
}

TEST(wmclient, zero_is_a_poll_not_a_sleep) {
    reset();
    wm_window_t w = window(12, 3);
    wm_wait_ms(&w, NULL, 0, 0);
    CHECK_EQ(last_timeout, 0);
}

TEST(wmclient, extra_descriptors_follow_the_event_pipe_and_negative_ones_are_skipped) {
    reset();
    wm_window_t w = window(12, 3);
    int extra[] = {5, -1, 9};
    wm_wait_ms(&w, extra, 3, 50);
    CHECK_EQ(last_count, 3);
    CHECK_EQ(last_fds[0], 12);
    CHECK_EQ(last_fds[1], 5);
    CHECK_EQ(last_fds[2], 9);
}

TEST(wmclient, a_window_with_no_event_pipe_still_waits_on_the_extras) {
    /* A client between compositors (M55) has evt_fd -1; a terminal with a
     * child running must still wake on the child's output. */
    reset();
    wm_window_t w = window(-1, 3);
    int extra[] = {5};
    wm_wait_ms(&w, extra, 1, 50);
    CHECK_EQ(last_count, 1);
    CHECK_EQ(last_fds[0], 5);
}

TEST(wmclient, more_extras_than_fit_are_dropped_rather_than_overrun) {
    reset();
    wm_window_t w = window(12, 3);
    int extra[12];
    for (int i = 0; i < 12; i++) {
        extra[i] = 100 + i;
    }
    wm_wait_ms(&w, extra, 12, 50);
    CHECK_EQ(last_count, 9); /* the pipe plus eight - the array in wm_wait_ms */
    CHECK_EQ(last_fds[8], 107);
}

TEST(wmclient, ready_is_one_and_the_deadline_is_zero) {
    reset();
    wm_window_t w = window(12, 3);
    waitfds_answer = 0;
    CHECK_EQ(wm_wait_ms(&w, NULL, 0, 50), 1);
    waitfds_answer = 2;
    CHECK_EQ(wm_wait_ms(&w, NULL, 0, 50), 1);
    waitfds_answer = -2;
    CHECK_EQ(wm_wait_ms(&w, NULL, 0, 50), 0);
    waitfds_answer = -1; /* a bad argument is "nothing ready" too - the caller loops and polls */
    CHECK_EQ(wm_wait_ms(&w, NULL, 0, 50), 0);
}

/* ---- wm_present --------------------------------------------------------- */

TEST(wmclient, present_sends_one_action_naming_this_window) {
    reset();
    wm_window_t w = window(12, 3);
    CHECK_EQ(wm_present(&w), 0);
    CHECK_EQ(pipe_open_calls, 1);
    CHECK_EQ(written_fd, 41); /* the action pipe's write end */
    CHECK_EQ(written_len, sizeof(wm_action_request_t));
    wm_action_request_t req;
    memcpy(&req, written, sizeof(req));
    CHECK_EQ(req.window_id, 3);
    CHECK_EQ(req.action, WM_ACTION_PRESENT);
    CHECK_EQ(req.value, 0);
}

TEST(wmclient, present_opens_the_action_pipe_once) {
    reset();
    wm_window_t w = window(12, 3);
    wm_present(&w);
    wm_present(&w);
    wm_present(&w);
    CHECK_EQ(pipe_open_calls, 1);
}

TEST(wmclient, a_window_the_compositor_has_not_named_cannot_be_presented) {
    reset();
    wm_window_t w = window(12, -1);
    CHECK_EQ(wm_present(&w), -1);
    CHECK_EQ(written_len, 0);
    CHECK_EQ(wm_present(NULL), -1);
}
