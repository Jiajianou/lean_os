#include "check.h"

#include <setjmp.h>
#include <string.h>

#define sys_waitfds window_manager_test_waitfds
#define sys_uptime_ms window_manager_test_uptime_ms
#define sys_yield window_manager_test_yield
#define sys_getpid window_manager_test_getpid
#define sys_fcntl window_manager_test_fcntl
#include "../user_space/library/window_manager_client.c"
#undef sys_waitfds
#undef sys_uptime_ms
#undef sys_yield
#undef sys_getpid
#undef sys_fcntl

#define REQUEST_READ_END   60
#define REQUEST_WRITE_END  61
#define RESPONSE_READ_END  50
#define RESPONSE_WRITE_END 51
#define THIS_PID    42
#define OTHER_PID   43

static long now_ms;
static unsigned char response_pipe[256];
static size_t response_buffered;
static int response_nonblocking;
static int reply_stolen_before_read;
static int answer_request_number;
static int requests_sent;
static int blocked_on_an_empty_pipe;
static long shared_memory_answer;

long window_manager_test_uptime_ms(void) { return now_ms; }
long window_manager_test_yield(void) { now_ms++; return 0; }
long window_manager_test_getpid(void) { return THIS_PID; }

long window_manager_test_fcntl(int fd, int command, long arg) {
    if (fd == RESPONSE_READ_END && command == F_SETFL_COMMAND) {
        response_nonblocking = (arg & OS_NONBLOCK_BIT) != 0;
    }
    return 0;
}

static void compositor_replies_to(int32_t pid) {
    window_manager_create_response_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.window_id = pid == THIS_PID ? 1 : 0;
    reply.shared_memory_id = 7;
    reply.width = 1024;
    reply.height = 768;
    reply.compositor_pid = 30;
    reply.client_pid = pid;
    memcpy(response_pipe + response_buffered, &reply, sizeof(reply));
    response_buffered += sizeof(reply);
}

static int last_file_descriptors[16];
static int last_count;
static int last_timeout;
static long waitfds_answer;

long window_manager_test_waitfds(const int *file_descriptors, int count, int timeout_ms) {
    last_count = count;
    last_timeout = timeout_ms;
    /* A wait that finds nothing takes its timeout. Since M203 the connect
       loops sleep here instead of yielding, and a clock that did not move
       made their deadlines unreachable. */
    if (waitfds_answer < 0 && timeout_ms > 0) {
        now_ms += timeout_ms;
    }
    for (int i = 0; i < count && i < 16; i++) {
        last_file_descriptors[i] = file_descriptors[i];
    }
    return waitfds_answer;
}

static unsigned char written[256];
static size_t written_length;
static int written_file_descriptor;
static window_manager_action_request_t actions[8];
static int action_count;

long sys_write(int fd, const void *buffer, size_t length) {
    if (fd == REQUEST_WRITE_END) {
        requests_sent++;
        if (requests_sent == answer_request_number) {
            compositor_replies_to(THIS_PID);
        }
        return (long)length;
    }
    if (fd == RESPONSE_WRITE_END) {
        memcpy(response_pipe + response_buffered, buffer, length);
        response_buffered += length;
        return (long)length;
    }
    written_file_descriptor = fd;
    written_length = length < sizeof(written) ? length : sizeof(written);
    memcpy(written, buffer, written_length);
    if (length == sizeof(window_manager_action_request_t) && action_count < 8) {
        memcpy(&actions[action_count++], buffer, length);
    }
    return (long)length;
}

static int pipe_open_calls;

long sys_pipe_open(const char *name, int file_descriptors_out[2]) {
    (void)name;
    pipe_open_calls++;
    if (strcmp(name, WINDOW_MANAGER_REQUEST_PIPE) == 0) {
        file_descriptors_out[0] = REQUEST_READ_END;
        file_descriptors_out[1] = REQUEST_WRITE_END;
        return 0;
    }
    if (strcmp(name, WINDOW_MANAGER_RESPONSE_PIPE) == 0) {
        file_descriptors_out[0] = RESPONSE_READ_END;
        file_descriptors_out[1] = RESPONSE_WRITE_END;
        return 0;
    }
    file_descriptors_out[0] = 40;
    file_descriptors_out[1] = 41;
    return 0;
}

long sys_read(int fd, void *buffer, size_t length) {
    if (fd != RESPONSE_READ_END) {
        return -1;
    }
    if (reply_stolen_before_read) {
        reply_stolen_before_read = 0;
        response_buffered = 0;
    }
    if (response_buffered == 0) {
        if (response_nonblocking) {
            return -OS_ERROR_AGAIN;
        }
        blocked_on_an_empty_pipe = 1;
        return -1;
    }
    size_t n = length < response_buffered ? length : response_buffered;
    memcpy(buffer, response_pipe, n);
    memmove(response_pipe, response_pipe + n, response_buffered - n);
    response_buffered -= n;
    return (long)n;
}

long sys_pipe_poll(int fd) {
    return fd == RESPONSE_READ_END ? (long)response_buffered : 0;
}

long sys_task_alive(long pid) { (void)pid; return 1; }
long sys_shared_memory_map(long id) { (void)id; return shared_memory_answer; }
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
    action_count = 0;
    written_file_descriptor = -1;
    pipe_open_calls = 0;
    action_file_descriptors[0] = -1;
    action_file_descriptors[1] = -1;
    now_ms = 0;
    response_buffered = 0;
    response_nonblocking = 0;
    reply_stolen_before_read = 0;
    answer_request_number = 0;
    requests_sent = 0;
    blocked_on_an_empty_pipe = 0;
    shared_memory_answer = -1;
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

TEST(window_manager_client, a_reply_another_client_takes_first_does_not_park_this_one) {
    reset();
    shared_memory_answer = 0x10000;
    compositor_replies_to(OTHER_PID);
    reply_stolen_before_read = 1;
    answer_request_number = 2;
    window_manager_window_t w;
    memset(&w, 0, sizeof(w));
    CHECK_EQ(window_manager_connect_desktop(&w), 0);
    CHECK_EQ(blocked_on_an_empty_pipe, 0);
    CHECK_EQ(requests_sent, 2);
    CHECK_EQ(w.window_id, 1);
    CHECK_EQ(w.width, 1024u);
}

TEST(window_manager_client, a_reply_for_another_client_goes_back_for_it) {
    reset();
    shared_memory_answer = 0x10000;
    compositor_replies_to(OTHER_PID);
    answer_request_number = 1;
    window_manager_window_t w;
    memset(&w, 0, sizeof(w));
    CHECK_EQ(window_manager_connect_desktop(&w), 0);
    CHECK_EQ(w.window_id, 1);
    CHECK_EQ(response_buffered, sizeof(window_manager_create_response_t));
    window_manager_create_response_t left;
    memcpy(&left, response_pipe, sizeof(left));
    CHECK_EQ(left.client_pid, OTHER_PID);
}

TEST(window_manager_client, a_compositor_that_never_answers_is_given_up_on) {
    reset();
    window_manager_window_t w;
    memset(&w, 0, sizeof(w));
    CHECK_EQ(window_manager_connect_desktop(&w), -1);
    CHECK_EQ(requests_sent, WINDOW_MANAGER_CONNECT_ATTEMPTS);
    CHECK_EQ(blocked_on_an_empty_pipe, 0);
}

/* M200: a present of part of a window is two actions - where, then how
   big - and a present that covers the window is still the one. */
TEST(window_manager_client, a_present_of_the_whole_window_is_one_present) {
    reset();
    window_manager_window_t w = window(12, 3);
    w.width = 400;
    w.height = 300;
    CHECK_EQ(window_manager_present_rect(&w, 0, 0, 400, 300), 0);
    CHECK_EQ(action_count, 1);
    CHECK_EQ(actions[0].action, WINDOW_MANAGER_ACTION_PRESENT);
    CHECK_EQ(actions[0].window_id, 3);
}

TEST(window_manager_client, a_present_of_part_of_a_window_names_the_part) {
    reset();
    window_manager_window_t w = window(12, 3);
    w.width = 400;
    w.height = 300;
    CHECK_EQ(window_manager_present_rect(&w, 17, 250, 9, 16), 0);
    CHECK_EQ(action_count, 2);
    CHECK_EQ(actions[0].action, WINDOW_MANAGER_ACTION_PRESENT_ORIGIN);
    CHECK_EQ(actions[1].action, WINDOW_MANAGER_ACTION_PRESENT_SIZE);
    CHECK_EQ(actions[0].window_id, 3);
    CHECK_EQ(actions[1].window_id, 3);
    CHECK_EQ(window_manager_pair_first(actions[0].value), 17);
    CHECK_EQ(window_manager_pair_second(actions[0].value), 250);
    CHECK_EQ(window_manager_pair_first(actions[1].value), 9);
    CHECK_EQ(window_manager_pair_second(actions[1].value), 16);
}

TEST(window_manager_client, a_part_hanging_off_the_top_left_is_cut_to_the_window) {
    reset();
    window_manager_window_t w = window(12, 3);
    w.width = 400;
    w.height = 300;
    CHECK_EQ(window_manager_present_rect(&w, -5, -10, 20, 30), 0);
    CHECK_EQ(action_count, 2);
    CHECK_EQ(window_manager_pair_first(actions[0].value), 0);
    CHECK_EQ(window_manager_pair_second(actions[0].value), 0);
    CHECK_EQ(window_manager_pair_first(actions[1].value), 15);
    CHECK_EQ(window_manager_pair_second(actions[1].value), 20);
    reset();
    CHECK_EQ(window_manager_present_rect(&w, -50, 10, 20, 30), 0);
    CHECK_EQ(action_count, 0);
}

static jmp_buf exited;
static int exit_status_seen;
static int actions_before_exit;

void sys_exit(int code) {
    exit_status_seen = code;
    actions_before_exit = action_count;
    longjmp(exited, 1);
}

TEST(window_manager_client, quitting_says_so_for_this_window_before_it_exits) {
    reset();
    window_manager_window_t w = window(12, 5);
    exit_status_seen = -1;
    actions_before_exit = -1;
    if (setjmp(exited) == 0) {
        window_manager_quit(&w, 0);
    }
    CHECK_EQ(exit_status_seen, 0);
    CHECK_EQ(actions_before_exit, 1);
    CHECK_EQ(actions[0].action, WINDOW_MANAGER_ACTION_QUITTING);
    CHECK_EQ(actions[0].window_id, 5);
}

TEST(window_manager_client, a_window_never_named_quits_without_naming_one) {
    reset();
    window_manager_window_t w = window(12, -1);
    exit_status_seen = -1;
    if (setjmp(exited) == 0) {
        window_manager_quit(&w, 3);
    }
    CHECK_EQ(exit_status_seen, 3);
    CHECK_EQ(actions_before_exit, 0);
}
