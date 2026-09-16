#include "check.h"

/* M157. The 4.2BSD timeval macros, which fontconfig's cache uses and this
   libc did not have. Their whole job is to behave the way every other
   implementation of themselves behaves, so the host's own are the oracle -
   which means both sets have to be in one translation unit.

   That is not free. The host's <sys/time.h> has already defined struct
   timeval (whose tv_usec is 32 bits on macOS and 64 here, so they are not
   the same type) and declared gettimeofday, so this libc's header is
   included with those six names moved aside. What is under test is the
   MACROS, and they name only tv_sec and tv_usec - so they work on the host's
   struct unchanged, which is the whole reason this is possible. */

#include <sys/time.h>

#define HOST_WRAPPER(name, op)                                              \
    static void name(long as, long au, long bs, long bu,                    \
                     long *result_seconds, long *result_microseconds) {     \
        struct timeval a, b, r;                                             \
        a.tv_sec = (time_t)as; a.tv_usec = (suseconds_t)au;                 \
        b.tv_sec = (time_t)bs; b.tv_usec = (suseconds_t)bu;                 \
        op(&a, &b, &r);                                                     \
        *result_seconds = (long)r.tv_sec;                                   \
        *result_microseconds = (long)r.tv_usec;                             \
    }

HOST_WRAPPER(host_add, timeradd)
HOST_WRAPPER(host_subtract, timersub)

static int host_compare_less(long as, long au, long bs, long bu) {
    struct timeval a, b;
    a.tv_sec = (time_t)as; a.tv_usec = (suseconds_t)au;
    b.tv_sec = (time_t)bs; b.tv_usec = (suseconds_t)bu;
    return timercmp(&a, &b, <) ? 1 : 0;
}

#undef timerisset
#undef timerclear
#undef timercmp
#undef timeradd
#undef timersub

#define timeval        leanos_timeval
#define timezone       leanos_timezone
#define gettimeofday   leanos_gettimeofday
#define settimeofday   leanos_settimeofday
#define utimes         leanos_utimes
#define futimes        leanos_futimes
#include "../user_space/libc/include/sys/time.h"
#undef timeval
#undef timezone
#undef gettimeofday
#undef settimeofday
#undef utimes
#undef futimes

/* Everything below uses the HOST's struct timeval and THIS LIBC's macros. */

static struct timeval tv(long seconds, long microseconds) {
    struct timeval t;
    t.tv_sec = (time_t)seconds;
    t.tv_usec = (suseconds_t)microseconds;
    return t;
}

TEST(timeval_macros, isset_and_clear) {
    struct timeval zero = tv(0, 0);
    struct timeval seconds_only = tv(5, 0);
    struct timeval microseconds_only = tv(0, 5);
    CHECK(!timerisset(&zero));
    CHECK(timerisset(&seconds_only));
    CHECK(timerisset(&microseconds_only));

    struct timeval dirty = tv(9, 9);
    timerclear(&dirty);
    CHECK_EQ((long)dirty.tv_sec, 0L);
    CHECK_EQ((long)dirty.tv_usec, 0L);
    CHECK(!timerisset(&dirty));
}

TEST(timeval_macros, compare_orders_by_seconds_then_microseconds) {
    struct timeval early = tv(5, 100);
    struct timeval late = tv(5, 200);
    struct timeval later = tv(6, 0);

    CHECK(timercmp(&early, &late, <));
    CHECK(!timercmp(&late, &early, <));
    CHECK(timercmp(&late, &early, >));
    CHECK(timercmp(&early, &early, ==));
    CHECK(!timercmp(&early, &late, ==));
    CHECK(timercmp(&early, &late, !=));

    /* A whole second later beats more microseconds, which is what a
       comparison that reached tv_usec first would get wrong. */
    CHECK(timercmp(&late, &later, <));
    CHECK(timercmp(&later, &late, >));
}

/* fccache.c writes exactly this - a timercmp in the condition of an if with
   no braces, with an && beside it - and it is where fontconfig stopped. */
TEST(timeval_macros, compare_sits_inside_an_expression) {
    struct timeval a = tv(1, 0);
    struct timeval b = tv(2, 0);
    int reached = 0;
    if (timercmp(&a, &b, <) && !timercmp(&b, &a, <))
        reached = 1;
    CHECK_EQ(reached, 1);
    CHECK_EQ(timercmp(&a, &b, <) ? 10 : 20, 10);
}

static const struct { long as, au, bs, bu; } CASES[] = {
    { 0, 0, 0, 0 },
    { 1, 500000, 2, 500000 },
    { 1, 999999, 0, 1 },
    { 5, 0, 3, 999999 },
    { 0, 1, 0, 2 },
    { 100, 250000, 0, 750000 },
    { 7, 123456, 7, 123456 },
    { 2, 0, 1, 999999 },
    { 1000000, 999999, 1, 999999 },
    { 0, 0, 9, 999999 },
};
#define CASE_COUNT (sizeof(CASES) / sizeof(CASES[0]))

TEST(timeval_macros, add_and_subtract_agree_with_the_host) {
    for (unsigned i = 0; i < CASE_COUNT; i++) {
        struct timeval a = tv(CASES[i].as, CASES[i].au);
        struct timeval b = tv(CASES[i].bs, CASES[i].bu);
        struct timeval mine;
        long seconds, microseconds;

        timeradd(&a, &b, &mine);
        host_add(CASES[i].as, CASES[i].au, CASES[i].bs, CASES[i].bu,
                 &seconds, &microseconds);
        CHECK_EQ((long)mine.tv_sec, seconds);
        CHECK_EQ((long)mine.tv_usec, microseconds);
        /* And normalised, which is what the carry is for. */
        CHECK((long)mine.tv_usec >= 0 && (long)mine.tv_usec < 1000000);

        timersub(&a, &b, &mine);
        host_subtract(CASES[i].as, CASES[i].au, CASES[i].bs, CASES[i].bu,
                      &seconds, &microseconds);
        CHECK_EQ((long)mine.tv_sec, seconds);
        CHECK_EQ((long)mine.tv_usec, microseconds);
        CHECK((long)mine.tv_usec >= 0 && (long)mine.tv_usec < 1000000);
    }
}

TEST(timeval_macros, compare_agrees_with_the_host_over_every_pair) {
    int disagreements = 0;
    int trues = 0;
    for (unsigned i = 0; i < CASE_COUNT; i++) {
        for (unsigned j = 0; j < CASE_COUNT; j++) {
            struct timeval a = tv(CASES[i].as, CASES[i].au);
            struct timeval b = tv(CASES[j].as, CASES[j].bu);
            int mine = timercmp(&a, &b, <) ? 1 : 0;
            int theirs = host_compare_less(CASES[i].as, CASES[i].au,
                                           CASES[j].as, CASES[j].bu);
            if (mine != theirs) {
                disagreements++;
            }
            trues += mine;
        }
    }
    CHECK_EQ(disagreements, 0);
    /* A run in which the answer was never true would pass with the macro
       returning a constant, so the sweep has to reach both answers. */
    CHECK(trues > 0);
    CHECK(trues < (int)(CASE_COUNT * CASE_COUNT));
}

TEST(timeval_macros, a_result_is_one_timeval_and_not_the_one_after_it) {
    /* These macros name their arguments more than once - glibc's timerisset
       reads tvp twice and its timeradd writes through result three times -
       so an argument with a side effect in it is the CALLER's bug in every
       implementation there is, and a version here that avoided it would be
       the odd one out. This grades what is actually promised instead: the
       result is written and the timeval after it is not. The first draft of
       this test asserted single evaluation, passed `destination++`, and ASan
       reported the stack overrun that assumption produces. */
    struct timeval a = tv(1, 700000);
    struct timeval b = tv(2, 700000);
    struct timeval out[2];
    out[0] = tv(9, 9);
    out[1] = tv(9, 9);

    timeradd(&a, &b, &out[0]);
    CHECK_EQ((long)out[0].tv_sec, 4L);
    CHECK_EQ((long)out[0].tv_usec, 400000L);
    CHECK_EQ((long)out[1].tv_sec, 9L);
    CHECK_EQ((long)out[1].tv_usec, 9L);

    timersub(&a, &b, &out[1]);
    CHECK_EQ((long)out[1].tv_sec, -1L);
    CHECK_EQ((long)out[1].tv_usec, 0L);
    CHECK_EQ((long)out[0].tv_sec, 4L);
}
