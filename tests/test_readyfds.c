/* tests/test_readyfds.c - M119: the three objects a message pump is made
 * of, off the machine.
 *
 * What makes this tier the right one for these three is that all of them
 * are *state machines over time and counters*, and almost everything worth
 * knowing about them is a boundary a booted machine cannot be held still
 * at: what a timer reports at 999 ms and at 1000, what a counter does at
 * saturation, what an edge-triggered registration reports on the second
 * scan when nothing changed.
 *
 * kernel/ipc/timerfd.c takes the time as an argument and kernel/ipc/epoll.c
 * takes the readiness question as a function pointer, both for this reason
 * alone - so a test can ask about the nanosecond before a deadline, and can
 * script a descriptor that becomes writable without there being a
 * descriptor at all. The alternative is tcp.c's arrangement, where the
 * clock is a global and the tests pay for it with a fake PIT whose every
 * read advances time.
 */
#include "check.h"

#include "ipc/epoll.h"
#include "ipc/eventfd.h"
#include "ipc/timerfd.h"
#include "sched/sched.h" /* fd_slot_t, fd_retain/fd_release - the table's own wiring, graded at the bottom of this file */

#include <string.h>

#define MS 1000000ULL       /* one millisecond in nanoseconds */
#define TICK (10 * MS)      /* PIT_HZ is 100: the granularity everything rounds to */

/* ---- eventfd ---------------------------------------------------------- */

TEST(eventfd, a_counter_collects_and_a_read_takes_all_of_it) {
    eventfd_init();
    struct eventfd *e = eventfd_create(0, 0);
    REQUIRE(e != NULL);
    CHECK_EQ(eventfd_readable(e), 0);
    uint64_t v = 99;
    CHECK_EQ(eventfd_read(e, &v), -1); /* nothing yet, which is not an error */
    CHECK_EQ(v, 99);                   /* and nothing was written over */

    CHECK_EQ(eventfd_write(e, 1), 0);
    CHECK_EQ(eventfd_write(e, 4), 0);
    CHECK_EQ(eventfd_readable(e), 1);
    /* Five, not two reads of one each: the counter is "how many times was I
     * poked since I last looked", which is what a wake-up flag wants and
     * the reason this is not a pipe. */
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
    CHECK_EQ(eventfd_read(e, &v), -1); /* and now it is empty */
    eventfd_unref(e);
    CHECK_EQ(eventfd_in_use(), 0);
}

TEST(eventfd, saturation_is_a_wait_and_the_two_illegal_values_are_not) {
    eventfd_init();
    struct eventfd *e = eventfd_create(EVENTFD_MAX_COUNT, 0);
    REQUIRE(e != NULL);
    CHECK_EQ(eventfd_writable(e), 0);
    /* -1 is "would block": a reader has to take some first, and a caller
     * that loops will get through. */
    CHECK_EQ(eventfd_write(e, 1), -1);
    /* -2 is "never": 0 is a no-op and all-ones is the value a read could
     * never report unambiguously, so neither is something to wait for.
     * Telling them apart is what stops syscall.c parking forever on a
     * write that can never succeed. */
    CHECK_EQ(eventfd_write(e, 0), -2);
    CHECK_EQ(eventfd_write(e, 0xFFFFFFFFFFFFFFFFULL), -2);
    uint64_t v = 0;
    CHECK_EQ(eventfd_read(e, &v), 0);
    CHECK_EQ(v, EVENTFD_MAX_COUNT);
    CHECK_EQ(eventfd_writable(e), 1);
    CHECK_EQ(eventfd_write(e, 1), 0);
    /* And an initial value past saturation is refused at creation rather
     * than clamped. */
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
    REQUIRE(all[0] != NULL); /* Q9's rule: it has to work again */
    for (int i = 0; i < EVENTFD_MAX; i++) {
        eventfd_unref(all[i]);
    }
    CHECK_EQ(eventfd_in_use(), 0);
}

/* ---- timerfd ---------------------------------------------------------- */

TEST(timerfd, a_one_shot_fires_once_at_the_time_it_was_asked_for) {
    timerfd_init();
    struct timerfd *t = timerfd_create(TIMERFD_CLOCK_MONOTONIC);
    REQUIRE(t != NULL);
    /* A fresh timer is disarmed and never becomes readable on its own,
     * which is what makes a blocking read on one a wait rather than a
     * spin. */
    CHECK_EQ(timerfd_readable(t, 0), 0);
    CHECK_EQ(timerfd_next_ms(t, 0), -1);

    CHECK_EQ(timerfd_settime(t, 1000 * MS, 0, 100 * MS, 0, NULL, NULL), 0);
    /* The nanosecond before, and the nanosecond of. This is the assertion
     * the whole design of this file is for: no machine can be held at
     * 1099.999 ms. */
    CHECK_EQ(timerfd_readable(t, 1099 * MS + 999999), 0);
    CHECK_EQ(timerfd_readable(t, 1100 * MS), 1);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 1100 * MS, &n), 0);
    CHECK_EQ(n, 1);
    /* Once. A one-shot is disarmed by firing, so a second read finds
     * nothing however much later it happens. */
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
    /* 100 ms later, a 10 ms timer has fired ten times. A reader that was
     * told "once" would have no way to know it was behind - which is the
     * whole reason Linux reports a count here and the reason this does. */
    CHECK_EQ(timerfd_read(t, 100 * MS, &n), 0);
    CHECK_EQ(n, 10);
    /* And the phase is kept rather than restarted from the read: the next
     * firing is at 110 ms, not at 110 ms after whenever somebody looked. */
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
    /* One nanosecond. The clock advances in 10 ms steps, so this fires on
     * the next tick - and the direction matters: rounded DOWN it would be
     * readable immediately and for ever, which is an event loop that spins
     * at 100% of a core. */
    CHECK_EQ(timerfd_settime(t, 0, 0, 1, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 0), 0);
    CHECK_EQ(timerfd_readable(t, TICK - 1), 0);
    CHECK_EQ(timerfd_readable(t, TICK), 1);
    /* And the same for an interval, which would otherwise be a divide by
     * zero or an infinite expiration count. */
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
    /* POSIX's rule, and the trap this test exists to pin: {0, 10ms} is a
     * DISARM. A program that meant "every 10 ms" and wrote this gets a
     * timer that never fires, and an implementation that guessed what it
     * meant would disagree with every other system. */
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
    CHECK_EQ(timerfd_readable(t, 50 * MS), 1); /* five firings nobody has read */
    uint64_t old_value = 0, old_interval = 0;
    CHECK_EQ(timerfd_settime(t, 50 * MS, 0, 1000 * MS, 0, &old_value, &old_interval), 0);
    /* What was left of the old setting, not what it was originally set to -
     * the only form that lets a caller put back a timer it borrowed. */
    CHECK_EQ(old_value, 10 * MS);
    CHECK_EQ(old_interval, 10 * MS);
    /* And the five firings are gone: a re-armed timer reporting the old
     * one's expirations is a timer reporting the past. */
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
    /* A program that computed "now + 5 ms" and was preempted for 10 has
     * asked for something reasonable, and refusing it would make every
     * absolute deadline a race the caller has to retry. */
    CHECK_EQ(timerfd_settime(t, 1000 * MS, 1, 500 * MS, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, 1000 * MS), 1);
    /* And one in the future is exactly where it was asked for, measured on
     * the same clock rather than as a distance. */
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
    /* 35 ms in, with nobody having read it: the next firing is at 40 ms, so
     * 5 ms away. Zero would mean disarmed, which is the one answer that
     * must not be given for an armed timer. */
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
    CHECK_EQ(timerfd_next_ms(t, 0), -1); /* disarmed: nothing to wait for */
    CHECK_EQ(timerfd_settime(t, 0, 0, 250 * MS, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_next_ms(t, 0), 250);
    CHECK_EQ(timerfd_next_ms(t, 200 * MS), 50);
    /* Past its deadline, or already fired: zero, which tells syscall.c not
     * to park at all. A negative here would be read as "never" and the
     * waiter would sleep through the expiry it was waiting for. */
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

/* ---- epoll ------------------------------------------------------------
 *
 * The readiness table every scan below is answered from. This is the seam
 * kernel/ipc/epoll.c's function pointer exists for: no descriptors, no
 * task, no kernel - just "what would fd 3 say right now", which is the
 * only thing the set management depends on.
 */
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

/* The object pointer a registration is made with. Never dereferenced here;
 * it is an identity, and the scan callback above ignores it - the stale
 * case is driven by `stale_fd` instead, because in the kernel it is
 * syscall.c that notices an identity has changed. */
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
    /* The cookie is the whole reason a pump uses epoll rather than poll:
     * it maps the event to its handler with no side table. */
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
    /* Nothing changed. The level one says so again - which is what makes a
     * caller that did not drain the descriptor correct - and the edge one
     * stays quiet, which is what makes a caller that did not drain it
     * hang. Both behaviours are the contract. */
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 8), 1);
    CHECK_EQ(out[0].data, 3);
    /* A new condition on the edge-triggered one is an edge. */
    scripted[4] = EPOLLIN | EPOLLOUT;
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_MOD, 4, obj_for(4),
                           EPOLLIN | EPOLLOUT | EPOLLET, 4), 0);
    int n = epoll_scan(ep, script_mask, NULL, out, 8);
    CHECK_EQ(n, 2);
    /* And when the condition goes away entirely, the edge re-arms. */
    scripted[4] = 0;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 8), 1); /* only fd 3, level */
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
    /* Still readable, and deliberately silent: this is how a pump hands a
     * descriptor to a worker thread without the next scan handing it to a
     * second one. */
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_watch_count(ep), 1); /* still in the set, just disarmed */
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_MOD, 5, obj_for(5), EPOLLIN | EPOLLONESHOT, 5), 0);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, errors_and_hangups_arrive_whether_or_not_they_were_asked_for) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    /* Asked about writability only. */
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 6, obj_for(6), EPOLLOUT, 6), 0);
    scripted[6] = EPOLLIN; /* readable, which it did not ask about */
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    scripted[6] = EPOLLIN | EPOLLHUP;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    /* The HUP and not the IN: a program is told what it asked about, plus
     * the two things it has to be told whatever it asked, and nothing
     * else. Without this a loop waiting to write to a dead peer waits
     * forever. */
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
    /* fd 7 was closed, and the number may already belong to something else.
     * Reporting it would hand the caller an event for a descriptor it no
     * longer has - or worse, for whatever took the slot. */
    stale_fd = 7;
    epoll_ev_t out[4];
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    CHECK_EQ(out[0].data, 8);
    CHECK_EQ(epoll_watch_count(ep), 1); /* and it is gone from the set, not merely skipped */
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

TEST(epoll, add_mod_and_del_refuse_what_they_should) {
    script_reset();
    struct epoll *ep = epoll_create_set();
    REQUIRE(ep != NULL);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLIN, 1), 0);
    /* EEXIST rather than a silent MOD: a caller that adds twice has lost
     * track of its own set, and the second cookie would replace the
     * first's handler. */
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, 3, obj_for(3), EPOLLOUT, 2), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_MOD, 9, obj_for(9), EPOLLIN, 1), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_DEL, 9, NULL, 0, 0), -1);
    CHECK_EQ(epoll_ctl_set(ep, 99, 3, obj_for(3), EPOLLIN, 1), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_ADD, -1, obj_for(3), EPOLLIN, 1), -1);
    CHECK_EQ(epoll_ctl_set(ep, EPOLL_CTL_DEL, 3, NULL, 0, 0), 0);
    CHECK_EQ(epoll_watch_count(ep), 0);
    /* And a deleted registration reports nothing even while its descriptor
     * is ready. */
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
    /* The rest are still there to be reported - a level-triggered set does
     * not lose the ones that did not fit, which is what makes a small
     * maxevents a throughput choice rather than a correctness one. */
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
    /* Which is what makes `epoll_wait` on an empty set a sleep - the same
     * answer SYS_waitfds gives for a count of zero (M88), and for the same
     * reason: a wait with nothing that could satisfy it early IS a sleep. */
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_scan(ep, NULL, NULL, out, 4), 0);  /* and a missing callback is not a crash */
    CHECK_EQ(epoll_scan(NULL, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 0), 0);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

/* ---- what the mutation harness found --------------------------------
 *
 * `make mutate` over the three files above broke them on purpose and
 * reported 50%, 58% and 86%. Its survivors fell into three groups, and
 * only the third is interesting:
 *
 *   - **null guards nothing called with null.** Every entry point in all
 *     three files refuses a null object, and nothing asserted that any of
 *     them did. syscall.c never passes one, which is exactly the argument
 *     for checking it here: the guard is what keeps the next caller - a
 *     loop over a descriptor table with a hole in it - from dereferencing
 *     zero in ring 0.
 *   - **a reference count nothing took twice.** `--refs <= 0` mutated to
 *     `<= 1` frees an object that somebody still holds, and every test
 *     above holds exactly one reference. A descriptor inherited across a
 *     spawn or duplicated by dup2 holds a second, which is the ordinary
 *     case on this machine.
 *   - **the rounding and the clock boundaries**, which is where a real
 *     defect would hide.
 */

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
    eventfd_unref(NULL); /* and the two that return nothing do not fault */
    eventfd_ref(NULL);

    struct eventfd *e = eventfd_create(1, 0);
    REQUIRE(e != NULL);
    CHECK_EQ(eventfd_read(e, NULL), -1); /* a place to put the answer is not optional */
    eventfd_unref(e);

    CHECK_EQ(timerfd_read(NULL, 0, &v), -1);
    CHECK_EQ(timerfd_readable(NULL, 0), 0);
    CHECK_EQ(timerfd_next_ms(NULL, 0), -1);
    CHECK_EQ(timerfd_settime(NULL, 0, 0, 1, 0, NULL, NULL), -1);
    CHECK_EQ(timerfd_clock(NULL), -1);
    timerfd_unref(NULL);
    timerfd_ref(NULL);
    /* gettime's out-parameters are zeroed even for a null timer, which is
     * what stops a caller printing a stack value as a deadline. */
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
    /* Which is the ordinary case on this machine rather than a corner: a
     * descriptor inherited across SYS_spawn or duplicated by SYS_dup2 is a
     * second reference, and an object freed while one of them still names
     * it is a use-after-free in ring 0. */
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
    /* All three still exist, and still work - the counters say so, and so
     * does using them. */
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
    /* Exactly one tick: already a multiple, so rounding must leave it
     * alone. A round-up applied unconditionally would push every deadline
     * out by a full tick, which is a 10 ms error on every timer in the
     * system and exactly the kind of thing nobody notices. */
    CHECK_EQ(timerfd_settime(t, 0, 0, TICK, 0, NULL, NULL), 0);
    CHECK_EQ(timerfd_readable(t, TICK - 1), 0);
    CHECK_EQ(timerfd_readable(t, TICK), 1);
    /* And the interval likewise: 2 ticks stays 2 ticks, so ten intervals
     * is twenty ticks and not thirty. */
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
    /* Armed, and not yet fired: the time remaining. */
    timerfd_gettime(t, 5 * MS, &value, &interval);
    CHECK_EQ(value, 5 * MS);
    CHECK_EQ(interval, 0);
    uint64_t n = 0;
    CHECK_EQ(timerfd_read(t, 10 * MS, &n), 0);
    CHECK_EQ(n, 1);
    /* Fired, so disarmed, so zero - which is the one case where "disarmed"
     * and "nothing left" are the same answer, and the expiration count is
     * where the difference lives. */
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
    /* Several scans with nothing ready, which is the ordinary state of a
     * pump's set - and then readiness. The edge has to fire: a
     * registration whose "last reported" state were disturbed by the empty
     * scans would either miss this or report it twice. */
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    scripted[3] = EPOLLIN;
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 1);
    CHECK_EQ(epoll_scan(ep, script_mask, NULL, out, 4), 0);
    epoll_unref(ep);
    CHECK_EQ(epoll_in_use(), 0);
}

/* ---- the descriptor table's own wiring --------------------------------
 *
 * Three new `fd_type_t` values reached kernel/sched/sched.c in M119, and
 * the scheduler does exactly one thing with each: takes a reference when a
 * task inherits a descriptor and drops it when the task dies. That is six
 * lines, it is the difference between a process exiting cleanly and leaking
 * a kernel object per descriptor, and nothing would have noticed if one of
 * the three cases had been mistyped - the union means `slot->event` and
 * `slot->timer` are the same bits, so a copy-paste error that unref'd the
 * wrong KIND would compile and run.
 *
 * Q13 built fake_kernel_objects.c to grade exactly this for pipes, files
 * and sockets. These three are real objects rather than fakes, so the
 * assertion is on their own live counts instead.
 */
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

    /* A spawn copying its parent's table, or a dup2. */
    for (int i = 0; i < 3; i++) {
        fd_retain(&slots[i]);
    }
    CHECK_EQ(eventfd_in_use(), 1);
    CHECK_EQ(timerfd_in_use(), 1);
    CHECK_EQ(epoll_in_use(), 1);

    /* The child exits: one reference each goes, and all three objects have
     * to survive - this is the half that a mistyped case would break
     * silently, since the union makes every kind the same bits. */
    fd_slot_t copies[3];
    memcpy(copies, slots, sizeof(copies));
    for (int i = 0; i < 3; i++) {
        fd_release(&copies[i]);
        CHECK(copies[i].type == FD_NONE); /* and the slot is empty afterwards */
    }
    CHECK_EQ(eventfd_in_use(), 1);
    CHECK_EQ(timerfd_in_use(), 1);
    CHECK_EQ(epoll_in_use(), 1);

    /* And the parent exits. */
    for (int i = 0; i < 3; i++) {
        fd_release(&slots[i]);
    }
    CHECK_EQ(eventfd_in_use(), 0);
    CHECK_EQ(timerfd_in_use(), 0);
    CHECK_EQ(epoll_in_use(), 0);
}
