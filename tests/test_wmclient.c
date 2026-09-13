#include "check.h"

#include <string.h>

#define sys_waitfds window_manager_test_waitfds
#include "../user_space/library/window_manager_client.c"
#undef sys_waitfds

static int last_file_descriptors[16];
static int last_count;
static int last_timeout;
static long waitfds_answer;

long window_manager_test_waitfds(const int *file_descriptors, int count, int timeout_ms) {
    last_count = count;
    last_timeout = timeout_ms;
    for (int i = 0; i < count && i < 16; i++) {
        last_file_descriptors[i] = file_descriptors[i];
    }
    return waitfds_answer;
}

static unsigned char written[256];
static size_t written_length;
static int written_file_descriptor;

long sys_write(int fd, const void *buffer, size_t length) {
    written_file_descriptor = fd;
    written_length = length < sizeof(written) ? length : sizeof(written);
    memcpy(written, buffer, written_length);
    return (long)length;
}

static int pipe_open_calls;

long sys_pipe_open(const char *name, int file_descriptors_out[2]) {
    (void)name;
    pipe_open_calls++;
    file_descriptors_out[0] = 40;
    file_descriptors_out[1] = 41;
    return 0;
}

long sys_read(int fd, void *buffer, size_t length) { (void)fd; (void)buffer; (void)length; return -1; }
long sys_pipe_poll(int fd) { (void)fd; return 0; }
long sys_task_alive(long pid) { (void)pid; return 1; }
long sys_shared_memory_map(long id) { (void)id; return -1; }
long sys_shared_memory_unmap(void *vaddr, unsigned long bytes) { (void)vaddr; (void)bytes; return 0; }

static window_manager_window_t window(int evt_file_descriptor, int32_t id) {
    window_manager_window_t w;
    memset(&w, 0, sizeof(w));
    w.evt_file_descriptor = evt_file_descriptor;
    w.window_id = id;
    w.compositor_pid = -1;
    return w;
}

static void reset(void) {
    memset(last_file_descriptors, 0, sizeof(last_file_descriptors));
    last_count = -1;
    last_timeout = -99;
    waitfds_answer = -2;
    written_length = 0;
    written_file_descriptor = -1;
    pipe_open_calls = 0;
    action_file_descriptors[0] = -1;
    action_file_descriptors[1] = -1;
}

TEST(window_manager_client, the_event_pipe_is_waited_on_for_exactly_the_time_asked) {
    reset();
    window_manager_window_t w = window(12, 3);
    CHECK_EQ(window_manager_wait_ms(&w, NULL, 0, 30), 0);
    CHECK_EQ(last_count, 1);
    CHECK_EQ(last_file_descriptors[0], 12);
    CHECK_EQ(last_timeout, 30);
}

TEST(window_manager_client, no_deadline_and_long_deadlines_are_capped_at_the_liveness_period) {
    reset();
    window_manager_window_t w = window(12, 3);
    window_manager_wait_ms(&w, NULL, 0, -1);
    CHECK_EQ(last_timeout, WINDOW_MANAGER_WAIT_CAP_MS);
    window_manager_wait_ms(&w, NULL, 0, 10000);
    CHECK_EQ(last_timeout, WINDOW_MANAGER_WAIT_CAP_MS);
    window_manager_wait_ms(&w, NULL, 0, WINDOW_MANAGER_WAIT_CAP_MS);
    CHECK_EQ(last_timeout, WINDOW_MANAGER_WAIT_CAP_MS);
}

TEST(window_manager_client, zero_is_a_poll_not_a_sleep) {
    reset();
    window_manager_window_t w = window(12, 3);
    window_manager_wait_ms(&w, NULL, 0, 0);
    CHECK_EQ(last_timeout, 0);
}

TEST(window_manager_client, extra_descriptors_follow_the_event_pipe_and_negative_ones_are_skipped) {
    reset();
    window_manager_window_t w = window(12, 3);
    int extra[] = {5, -1, 9};
    window_manager_wait_ms(&w, extra, 3, 50);
    CHECK_EQ(last_count, 3);
    CHECK_EQ(last_file_descriptors[0], 12);
    CHECK_EQ(last_file_descriptors[1], 5);
    CHECK_EQ(last_file_descriptors[2], 9);
}

TEST(window_manager_client, a_window_with_no_event_pipe_still_waits_on_the_extras) {
    reset();
    window_manager_window_t w = window(-1, 3);
    int extra[] = {5};
    window_manager_wait_ms(&w, extra, 1, 50);
    CHECK_EQ(last_count, 1);
    CHECK_EQ(last_file_descriptors[0], 5);
}

TEST(window_manager_client, more_extras_than_fit_are_dropped_rather_than_overrun) {
    reset();
    window_manager_window_t w = window(12, 3);
    int extra[12];
    for (int i = 0; i < 12; i++) {
        extra[i] = 100 + i;
    }
    window_manager_wait_ms(&w, extra, 12, 50);
    CHECK_EQ(last_count, 9);
    CHECK_EQ(last_file_descriptors[8], 107);
}

TEST(window_manager_client, ready_is_one_and_the_deadline_is_zero) {
    reset();
    window_manager_window_t w = window(12, 3);
    waitfds_answer = 0;
    CHECK_EQ(window_manager_wait_ms(&w, NULL, 0, 50), 1);
    waitfds_answer = 2;
    CHECK_EQ(window_manager_wait_ms(&w, NULL, 0, 50), 1);
    waitfds_answer = -2;
    CHECK_EQ(window_manager_wait_ms(&w, NULL, 0, 50), 0);
    waitfds_answer = -1;
    CHECK_EQ(window_manager_wait_ms(&w, NULL, 0, 50), 0);
}

TEST(window_manager_client, present_sends_one_action_naming_this_window) {
    reset();
    window_manager_window_t w = window(12, 3);
    CHECK_EQ(window_manager_present(&w), 0);
    CHECK_EQ(pipe_open_calls, 1);
    CHECK_EQ(written_file_descriptor, 41);
    CHECK_EQ(written_length, sizeof(window_manager_action_request_t));
    window_manager_action_request_t request;
    memcpy(&request, written, sizeof(request));
    CHECK_EQ(request.window_id, 3);
    CHECK_EQ(request.action, WINDOW_MANAGER_ACTION_PRESENT);
    CHECK_EQ(request.value, 0);
}

TEST(window_manager_client, present_opens_the_action_pipe_once) {
    reset();
    window_manager_window_t w = window(12, 3);
    window_manager_present(&w);
    window_manager_present(&w);
    window_manager_present(&w);
    CHECK_EQ(pipe_open_calls, 1);
}

TEST(window_manager_client, a_window_the_compositor_has_not_named_cannot_be_presented) {
    reset();
    window_manager_window_t w = window(12, -1);
    CHECK_EQ(window_manager_present(&w), -1);
    CHECK_EQ(written_length, 0);
    CHECK_EQ(window_manager_present(NULL), -1);
}
