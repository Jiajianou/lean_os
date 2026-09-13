#include "check.h"

#include "ipc/epoll.h"
#include "ipc/eventfd.h"
#include "ipc/timerfd.h"
#include "sched/sched.h"

#include <string.h>

#define MS 1000000ULL
#define TICK (10 * MS)

TEST(eventfd, a_counter_collects_and_a_read_takes_all_of_it) {
    eventfd_init();
    struct eventfd *e = eventfd_create(0, 0);
    REQUIRE(e != NULL);
    CHECK_EQ(eventfd_readable(e), 0);
    uint64_t v = 99;
    CHECK_EQ(eventfd_read(e, &v), -1);
    CHECK_EQ(v, 99);

    CHECK_EQ(eventfd_write(e, 1), 0);
    CHECK_EQ(eventfd_write(e, 4), 0);
    CHECK_EQ(eventfd_readable(e), 1);
    CHECK_EQ(eventfd_read(e, &v), 0);
    CHECK_EQ(v, 5);
    CHECK_EQ(eventfd_readable(e), 0);
    eventfd_unref(e);
    CHECK_EQ(eventfd_in_use(), 0);
}

TEST(eventfd, a_semaphore_takes_exactly_one) {
    eventfd_init();
    struct eventfd *e = eventfd_create(2, 1);
    REQUIRE(e != NULL);
    uint64_t v = 0;
    CHECK_EQ(eventfd_read(e, &v), 0);
    CHECK_EQ(v, 1);
    CHECK_EQ(eventfd_read(e, &v), 0);
    CHECK_EQ(v, 1);
    CHECK_EQ(eventfd_read(e, &v), -1);
    eventfd_unref(e);
    CHECK_EQ(eventfd_in_use(), 0);
}

TEST(eventfd, saturation_is_a_wait_and_the_two_illegal_values_are_not) {
    eventfd_init();
    struct eventfd *e = eventfd_create(EVENTFD_MAX_COUNT, 0);
    REQUIRE(e != NULL);
    CHECK_EQ(eventfd_writable(e), 0);
    CHECK_EQ(eventfd_write(e, 1), -1);
    CHECK_EQ(eventfd_write(e, 0), -2);
    CHECK_EQ(eventfd_write(e, 0xFFFFFFFFFFFFFFFFULL), -2);
    uint64_t v = 0;
    CHECK_EQ(eventfd_read(e, &v), 0);
    CHECK_EQ(v, EVENTFD_MAX_COUNT);
    CHECK_EQ(eventfd_writable(e), 1);
    CHECK_EQ(eventfd_write(e, 1), 0);
    CHECK(eventfd_create(0xFFFFFFFFFFFFFFFFULL, 0) == NULL);
    eventfd_unref(e);
    CHECK_EQ(eventfd_in_use(), 0);
}

TEST(eventfd, running_out_refuses_and_recovers) {
    eventfd_init();
    struct eventfd *all[EVENTFD_MAX];
    for (int i = 0; i < EVENTFD_MAX; i++) {
        all[i] = eventfd_create(0, 0);
        REQUIRE(all[i] != NULL);
    }
    CHECK(eventfd_create(0, 0) == NULL);
    eventfd_unref(all[0]);
    all[0] = eventfd_create(7, 0);
    REQUIRE(all[0] != NULL);
    for (int i = 0; i < EVENTFD_MAX; i++) {
        eventfd_unref(all[i]);
    }
    CHECK_EQ(eventfd_in_use(), 0);
}

TEST(timerfd, a_one_shot_fires_once_at_the_time_it_was_asked_for) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_readable(t, 0), 0);
    CHECK_EQ(timerfd_next_ms(t, 0), -1);

    CHECK_EQ(timerfd_settime(t, 1000 * MS, 0, 100 * MS, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 1099 * MS + 999999), 0);
    CHECK_EQ(timerfd_readable(t, 1100 * MS), 1);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 1100 * MS, &n), 0);
    CHECK_EQ(n, 1);
    CHECK_EQ(timerfd_read(t, 9999 * MS, &n), -1);
    CHECK_EQ(timerfd_next_ms(t, 9999 * MS), -1);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, a_periodic_timer_counts_what_nobody_read) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 0, 0, 10 * MS, 10 * MS, NULL, NULL), 0);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 100 * MS, &n), 0);
    CHECK_EQ(n, 10);
    CHECK_EQ(timerfd_readable(t, 105 * MS), 0);
    CHECK_EQ(timerfd_read(t, 110 * MS, &n), 0);
    CHECK_EQ(n, 1);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, a_request_under_one_tick_is_rounded_up_and_never_to_zero) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 0, 0, 1, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 0), 0);
    CHECK_EQ(timerfd_readable(t, TICK - 1), 0);
    CHECK_EQ(timerfd_readable(t, TICK), 1);
    CHECK_EQ(timerfd_settime(t, 0, 0, 1, 1, NULL, NULL), 0);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 10 * TICK, &n), 0);
    CHECK_EQ(n, 10);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, an_interval_with_no_value_is_off_and_not_periodic) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 0, 0, 0, 10 * MS, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 10000 * MS), 0);
    CHECK_EQ(timerfd_next_ms(t, 0), -1);
    uint64_t value = 1, interval = 1;
    timerfd_gettime(t, 0, &value, &interval);
    CHECK_EQ(value, 0);
    CHECK_EQ(interval, 0);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, arming_clears_what_nobody_read_and_reports_what_was_left) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 0, 0, 10 * MS, 10 * MS, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 50 * MS), 1);
    uint64_t old_value = 0, old_interval = 0;
    CHECK_EQ(timerfd_settime(t, 50 * MS, 0, 1000 * MS, 0, &old_value, &old_interval), 0);
    CHECK_EQ(old_value, 10 * MS);
    CHECK_EQ(old_interval, 10 * MS);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 50 * MS, &n), -1);
    CHECK_EQ(timerfd_readable(t, 1049 * MS), 0);
    CHECK_EQ(timerfd_readable(t, 1050 * MS), 1);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, an_absolute_deadline_in_the_past_fires_at_once) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 1000 * MS, 1, 500 * MS, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 1000 * MS), 1);
    CHECK_EQ(timerfd_settime(t, 1000 * MS, 1, 2000 * MS, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 1999 * MS), 0);
    CHECK_EQ(timerfd_readable(t, 2000 * MS), 1);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, gettime_tells_a_periodic_timer_from_a_disarmed_one) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 0, 0, 10 * MS, 10 * MS, NULL, NULL), 0);
    uint64_t value = 0, interval = 0;
    timerfd_gettime(t, 35 * MS, &value, &interval);
    CHECK_EQ(value, 5 * MS);
    CHECK_EQ(interval, 10 * MS);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, the_next_deadline_is_what_a_waiter_should_park_for) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_next_ms(t, 0), -1);
    CHECK_EQ(timerfd_settime(t, 0, 0, 250 * MS, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_next_ms(t, 0), 250);
    CHECK_EQ(timerfd_next_ms(t, 200 * MS), 50);
    CHECK_EQ(timerfd_next_ms(t, 250 * MS), 0);
    CHECK_EQ(timerfd_next_ms(t, 9000 * MS), 0);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, a_clock_that_does_not_exist_is_refused) {
    timerfd_init();
    CHECK(timerfd_create(2) == NULL);
    CHECK(timerfd_create(-1) == NULL);
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_REALTIME);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_clock(t), TIMERFD_CLOCK_REALTIME);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

static uint32_t scripted[16];
static int stale_fd = -1;

static uint32_t script_mask(void *ctx, int fd, const void *obj) {
    (void)ctx;
    (void)obj;
    if (fd == stale_fd) {
        return EPOLL_STALE;
    }
    if (fd < 0 || fd >= 16) {
        return 0;
    }
    return scripted[fd];
}

static void script_reset(void) {
    for (int i = 0; i < 16; i++) {
        scripted[i] = 0;
    }
    stale_fd = -1;
    epoll_init();
}

static const void *obj_for(int fd) {
    return (const void *)(uintptr_t)(0x2000 + fd);
}

TEST(epoll, a_set_remembers_its_registrations_and_carries_their_cookies) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_watch_count(ep), 0);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLIN, 0xAAAA), 0);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 4, obj_for(4), EPOLLIN, 0xBBBB), 0);
    CHECK_EQ(epoll_watch_count(ep), 2);

    epoll_ev_t out[8];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 8), 0);
    scripted[4] = EPOLLIN;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 8), 1);
    CHECK_EQ(out[0].events, EPOLLIN);
    CHECK_EQ(out[0].data, 0xBBBB);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, level_triggered_reports_again_and_edge_triggered_does_not) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLIN, 3), 0);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 4, obj_for(4), EPOLLIN | EPOLLET, 4), 0);
    scripted[3] = EPOLLIN;
    scripted[4] = EPOLLIN;
    epoll_ev_t out[8];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 8), 2);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 8), 1);
    CHECK_EQ(out[0].data, 3);
    scripted[4] = EPOLLIN | EPOLLOUT;
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_MOD, 4, obj_for(4),
                           EPOLLIN | EPOLLOUT | EPOLLET, 4), 0);
    int n = epoll_scan(ep, script_mask, NULL, out, 8);
    CHECK_EQ(n, 2);
    scripted[4] = 0;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 8), 1);
    scripted[4] = EPOLLIN;
    n = epoll_scan(ep, script_mask, NULL, out, 8);
    CHECK_EQ(n, 2);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, one_shot_fires_once_and_a_mod_re_arms_it) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 5, obj_for(5), EPOLLIN | EPOLLONESHOT, 5), 0);
    scripted[5] = EPOLLIN;
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_watch_count(ep), 1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_MOD, 5, obj_for(5), EPOLLIN | EPOLLONESHOT, 5), 0);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, errors_and_hangups_arrive_whether_or_not_they_were_asked_for) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 6, obj_for(6), EPOLLOUT, 6), 0);
    scripted[6] = EPOLLIN;
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    scripted[6] = EPOLLIN | EPOLLHUP;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    CHECK_EQ(out[0].events, EPOLLHUP);
    scripted[6] = EPOLLERR;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    CHECK_EQ(out[0].events, EPOLLERR);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, a_stale_registration_is_dropped_rather_than_reported) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 7, obj_for(7), EPOLLIN, 7), 0);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 8, obj_for(8), EPOLLIN, 8), 0);
    scripted[7] = EPOLLIN;
    scripted[8] = EPOLLIN;
    stale_fd = 7;
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    CHECK_EQ(out[0].data, 8);
    CHECK_EQ(epoll_watch_count(ep), 1);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, add_mod_and_del_refuse_what_they_should) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLIN, 1), 0);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLOUT, 2), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_MOD, 9, obj_for(9), EPOLLIN, 1), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_DEL, 9, NULL, 0, 0), -1);
    CHECK_EQ(epoll_ctl_set(ep, 99, 3, obj_for(3), EPOLLIN, 1), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, -1, obj_for(3), EPOLLIN, 1), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_DEL, 3, NULL, 0, 0), 0);
    CHECK_EQ(epoll_watch_count(ep), 0);
    scripted[3] = EPOLLIN;
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, a_full_set_refuses_and_recovers) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
        CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 100 + i, obj_for(i), EPOLLIN, (uint64_t)i), 0);
    }
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 999, obj_for(99), EPOLLIN, 99), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_DEL, 100, NULL, 0, 0), 0);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 999, obj_for(99), EPOLLIN, 99), 0);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, a_scan_returns_no_more_than_it_was_offered_room_for) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, i, obj_for(i), EPOLLIN, (uint64_t)i), 0);
        scripted[i] = EPOLLIN;
    }
    epoll_ev_t out[2];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 2), 2);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 2), 2);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 1), 1);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, running_out_of_sets_refuses_and_recovers) {
    script_reset();
    struct epoll *all[EPOLL_MAX];
    for (int i = 0; i < EPOLL_MAX; i++) {
        all[i] = epoll_create_set();
        REQUIRE(all[i] != NULL);
    }
    CHECK(epoll_create_set() == NULL);
    epoll_unref(all[0]);
    all[0] = epoll_create_set();
    REQUIRE(all[0] != NULL);
    for (int i = 0; i < EPOLL_MAX; i++) {
        epoll_unref(all[i]);
    }
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, a_set_with_nothing_in_it_is_a_timeout_and_not_an_error) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_scan(ep, NULL, NULL, out, 4), 0);
    CHECK_EQ(epoll_scan(NULL, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 0), 0);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(readyfds, every_call_refuses_a_null_object) {
    eventfd_init();
    timerfd_init();
    epoll_init();
    uint64_t v = 1234;
    CHECK_EQ(eventfd_read(NULL, &v), -1);
    CHECK_EQ(v, 1234);
    CHECK_EQ(eventfd_write(NULL, 1), -1);
    CHECK_EQ(eventfd_readable(NULL), 0);
    CHECK_EQ(eventfd_writable(NULL), 0);
    eventfd_unref(NULL);
    eventfd_ref(NULL);

    struct eventfd *e = eventfd_create(1, 0);
    REQUIRE(e != NULL);
    CHECK_EQ(eventfd_read(e, NULL), -1);
    eventfd_unref(e);

    CHECK_EQ(timerfd_read(NULL, 0, &v), -1);
    CHECK_EQ(timerfd_readable(NULL, 0), 0);
    CHECK_EQ(timerfd_next_ms(NULL, 0), -1);
    CHECK_EQ(timerfd_settime(NULL, 0, 0, 1, 0, NULL, NULL), -1);
    CHECK_EQ(timerfd_clock(NULL), -1);
    timerfd_unref(NULL);
    timerfd_ref(NULL);
    uint64_t value = 7, interval = 7;
    timerfd_gettime(NULL, 0, &value, &interval);
    CHECK_EQ(value, 0);
    CHECK_EQ(interval, 0);
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_read(t, 0, NULL), -1);
    timerfd_unref(t);

    CHECK_EQ(epoll_ctl_set(NULL, EPOLL_CTL_ADD, 1, NULL, 0, 0), -1);
    CHECK_EQ(epoll_watch_count(NULL), 0);
    epoll_unref(NULL);
    epoll_ref(NULL);

    CHECK_EQ(eventfd_in_use(), 0);
    CHECK_EQ(timerfd_in_use(), 0);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(readyfds, a_second_reference_keeps_each_object_alive) {
    eventfd_init();
    timerfd_init();
    epoll_init();
    struct eventfd *e = eventfd_create(3, 0);
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    struct epoll *ep = epoll_create_set();
    REQUIRE(e != NULL);
    REQUIRE(t != NULL);
    REQUIRE(ep != NULL);
    eventfd_ref(e);
    timerfd_ref(t);
    epoll_ref(ep);
    eventfd_unref(e);
    timerfd_unref(t);
    epoll_unref(ep);
    CHECK_EQ(eventfd_in_use(), 1);
    CHECK_EQ(timerfd_in_use(), 1);
    CHECK_EQ(epoll_in_use(), 1);
    uint64_t v = 0;
    CHECK_EQ(eventfd_read(e, &v), 0);
    CHECK_EQ(v, 3);
    CHECK_EQ(timerfd_settime(t, 0, 0, 10 * MS, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 10 * MS), 1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLIN, 1), 0);
    CHECK_EQ(epoll_watch_count(ep), 1);
    eventfd_unref(e);
    timerfd_unref(t);
    epoll_unref(ep);
    CHECK_EQ(eventfd_in_use(), 0);
    CHECK_EQ(timerfd_in_use(), 0);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(timerfd, running_out_of_timers_refuses_and_recovers) {
    timerfd_init();
    struct timerfd *all[TIMERFD_MAX];
    for (int i = 0; i < TIMERFD_MAX; i++) {
        all[i] = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
        REQUIRE(all[i] != NULL);
    }
    CHECK(timerfd_create(TIMERFD_CLOCK_MONOTONIC) == NULL);
    timerfd_unref(all[0]);
    all[0] = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(all[0] != NULL);
    for (int i = 0; i < TIMERFD_MAX; i++) {
        timerfd_unref(all[i]);
    }
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, a_deadline_on_a_tick_boundary_is_not_moved) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 0, 0, TICK, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, TICK - 1), 0);
    CHECK_EQ(timerfd_readable(t, TICK), 1);
    CHECK_EQ(timerfd_settime(t, 0, 0, 2 * TICK, 2 * TICK, NULL, NULL), 0);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 20 * TICK, &n), 0);
    CHECK_EQ(n, 10);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(timerfd, gettime_reports_nothing_for_a_timer_that_has_fired) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    CHECK_EQ(timerfd_settime(t, 0, 0, 10 * MS, 0, NULL, NULL), 0);
    uint64_t value = 7, interval = 7;
    timerfd_gettime(t, 5 * MS, &value, &interval);
    CHECK_EQ(value, 5 * MS);
    CHECK_EQ(interval, 0);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 10 * MS, &n), 0);
    CHECK_EQ(n, 1);
    value = 7;
    timerfd_gettime(t, 10 * MS, &value, &interval);
    CHECK_EQ(value, 0);
    timerfd_unref(t);
    CHECK_EQ(timerfd_in_use(), 0);
}

TEST(epoll, an_edge_registration_that_was_never_ready_stays_armed) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLIN | EPOLLET, 3), 0);
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    scripted[3] = EPOLLIN;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(readyfds, the_descriptor_table_refs_and_releases_all_three_kinds) {
    eventfd_init();
    timerfd_init();
    epoll_init();
    fd_slot_t slots[3];
    memset(slots, 0, sizeof(slots));
    slots[0].type = FD_EVENT;
    slots[0].event = eventfd_create(0, 0);
    slots[1].type = FD_TIMER;
    slots[1].timer = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    slots[2].type = FD_EPOLL;
    slots[2].epoll = epoll_create_set();
    REQUIRE(slots[0].event != NULL);
    REQUIRE(slots[1].timer != NULL);
    REQUIRE(slots[2].epoll != NULL);

    for (int i = 0; i < 3; i++) {
        fd_retain(&slots[i]);
    }
    CHECK_EQ(eventfd_in_use(), 1);
    CHECK_EQ(timerfd_in_use(), 1);
    CHECK_EQ(epoll_in_use(), 1);

    fd_slot_t copies[3];
    memcpy(copies, slots, sizeof(copies));
    for (int i = 0; i < 3; i++) {
        fd_release(&copies[i]);
        CHECK(copies[i].type == FD_NONE);
    }
    CHECK_EQ(eventfd_in_use(), 1);
    CHECK_EQ(timerfd_in_use(), 1);
    CHECK_EQ(epoll_in_use(), 1);

    for (int i = 0; i < 3; i++) {
        fd_release(&slots[i]);
    }
    CHECK_EQ(eventfd_in_use(), 0);
    CHECK_EQ(timerfd_in_use(), 0);
    CHECK_EQ(epoll_in_use(), 0);
}
