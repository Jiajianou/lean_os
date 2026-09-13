#include "check.h"
#include "fakes/fakes.h"

#include "device/pty.h"
#include "device/tty.h"
#include "scheduler/scheduler.h"
#include "signal.h"

#include <string.h>

void q13_boot(void);
task_t *q13_spawn(const char *name);
void q13_kill(task_t *t);

static int fresh_pty(void) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);
    return n;
}

static void done_pty(int n) {
    pty_slave_closed(n);
    pty_master_closed(n);
    CHECK(!pty_valid(n));
}

TEST(pty, a_line_is_delivered_whole_only_on_enter) {
    int n = fresh_pty();
    char buf[64];

    pty_master_write(n, "abX\177c", 5);
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 0);
    CHECK_EQ(pty_slave_readable(n), 0);

    pty_master_write(n, "\n", 1);
    CHECK_EQ(pty_slave_readable(n), 1);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 4);
    CHECK(strcmp(buf, "abc\n") == 0);
    done_pty(n);
}

TEST(pty, the_echo_goes_to_the_master_not_to_the_slave) {
    int n = fresh_pty();
    char buf[64];

    pty_master_write(n, "hi\n", 3);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_master_read(n, buf, sizeof(buf)), 4);
    CHECK(strcmp(buf, "hi\r\n") == 0);

    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 3);
    CHECK(strcmp(buf, "hi\n") == 0);
    done_pty(n);
}

TEST(pty, onlcr_applies_to_the_slaves_output) {
    int n = fresh_pty();
    char buf[64];

    pty_slave_write(n, "a\nb", 3);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_master_read(n, buf, sizeof(buf)), 4);
    CHECK(strcmp(buf, "a\r\nb") == 0);

    tty_t *t = pty_tty(n);
    REQUIRE(t != NULL);
    t->tio.c_oflag = 0;
    pty_slave_write(n, "a\nb", 3);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_master_read(n, buf, sizeof(buf)), 3);
    CHECK(strcmp(buf, "a\nb") == 0);
    done_pty(n);
}

TEST(pty, control_c_at_the_master_signals_the_foreground_group) {
    q13_boot();
    int n = fresh_pty();
    tty_t *t = pty_tty(n);
    REQUIRE(t != NULL);

    task_t *fg = q13_spawn("fg");
    task_t *bg = q13_spawn("bg");
    REQUIRE(fg != NULL);
    REQUIRE(bg != NULL);
    fg->pgid = fg->id;
    bg->pgid = bg->id;
    t->sid = 7;
    t->fg_pgid = fg->id;

    pty_master_write(n, "\003", 1);
    CHECK_EQ(fg->pending_signal, SIGINT);
    CHECK_EQ(bg->pending_signal, 0);

    fg->pending_signal = 0;
    pty_master_write(n, "\032", 1);
    CHECK_EQ(fg->pending_stop, SIGTSTP);
    CHECK_EQ(bg->pending_stop, 0);

    char buf[8];
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 0);

    q13_kill(fg);
    q13_kill(bg);
    done_pty(n);
}

TEST(pty, an_unclaimed_terminal_signals_nobody) {
    q13_boot();
    int n = fresh_pty();
    task_t *any = q13_spawn("bystander");
    REQUIRE(any != NULL);
    any->pgid = 0;
    pty_master_write(n, "\003", 1);
    CHECK_EQ(any->pending_signal, 0);
    q13_kill(any);
    done_pty(n);
}

TEST(pty, closing_the_master_hangs_the_slave_up) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);

    char buf[8];
    CHECK_EQ(pty_slave_readable(n), 0);
    pty_master_closed(n);
    CHECK_EQ(pty_slave_readable(n), 1);
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 0);
    pty_slave_closed(n);
    CHECK(!pty_valid(n));
}

TEST(pty, a_hung_up_slave_still_reads_what_was_already_typed) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);

    pty_master_write(n, "last\n", 5);
    pty_master_closed(n);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 5);
    CHECK(strcmp(buf, "last\n") == 0);
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 0);
    pty_slave_closed(n);
}

TEST(pty, a_master_does_not_read_end_of_file_before_the_slave_opens) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    CHECK_EQ(pty_master_readable(n), 0);

    pty_slave_opened(n);
    CHECK_EQ(pty_master_readable(n), 0);
    pty_slave_closed(n);
    CHECK_EQ(pty_master_readable(n), 1);
    pty_master_closed(n);
    CHECK(!pty_valid(n));
}

TEST(pty, a_pair_survives_until_both_ends_close) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);
    pty_master_closed(n);
    CHECK(pty_valid(n));
    pty_slave_closed(n);
    CHECK(!pty_valid(n));

    n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);
    pty_slave_closed(n);
    CHECK(pty_valid(n));
    pty_master_closed(n);
    CHECK(!pty_valid(n));
}

TEST(pty, two_slaves_on_one_pair_both_have_to_close) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);
    pty_slave_opened(n);
    pty_slave_closed(n);
    CHECK_EQ(pty_master_readable(n), 0);
    pty_slave_closed(n);
    CHECK_EQ(pty_master_readable(n), 1);
    pty_master_closed(n);
    CHECK(!pty_valid(n));
}

TEST(pty, the_ninth_pty_is_refused_and_the_table_recovers) {
    int held[PTY_MAX];
    for (int i = 0; i < PTY_MAX; i++) {
        held[i] = pty_alloc();
        CHECK(held[i] >= 0);
    }
    CHECK_EQ(pty_alloc(), -1);
    for (int i = 0; i < PTY_MAX; i++) {
        pty_master_closed(held[i]);
    }
    int again = pty_alloc();
    CHECK(again >= 0);
    pty_master_closed(again);
}

TEST(pty, the_output_queue_drops_rather_than_overwriting) {
    int n = fresh_pty();
    char one[1];
    for (int i = 0; i < TTY_OUTBUF * 2; i++) {
        char c = (char)('a' + (i % 26));
        pty_slave_write(n, &c, 1);
    }
    tty_t *t = pty_tty(n);
    REQUIRE(t != NULL);
    CHECK_EQ(tty_out_readable(t), TTY_OUTBUF - 1);
    CHECK_EQ(pty_master_read(n, one, 1), 1);
    CHECK_EQ(one[0], 'a');
    done_pty(n);
}

TEST(pty, the_input_queue_wraps_without_losing_a_byte) {
    int n = fresh_pty();
    char buf[64];
    for (int round = 0; round < 400; round++) {
        char line[8];
        int len = 0;
        line[len++] = (char)('0' + (round % 10));
        line[len++] = (char)('a' + (round % 26));
        line[len++] = '\n';
        pty_master_write(n, line, (uint32_t)len);
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 3);
        CHECK_EQ(buf[0], '0' + (round % 10));
        CHECK_EQ(buf[1], 'a' + (round % 26));
        CHECK_EQ(buf[2], '\n');
        while (tty_out_readable(pty_tty(n)) > 0) {
            pty_master_read(n, buf, sizeof(buf));
        }
    }
    done_pty(n);
}

TEST(pty, a_line_longer_than_the_buffer_stops_accepting_rather_than_truncating) {
    int n = fresh_pty();
    static char big[TTY_LINE_MAX * 2];
    memset(big, 'x', sizeof(big));
    pty_master_write(n, big, sizeof(big));
    pty_master_write(n, "\n", 1);

    static char out[TTY_LINE_MAX * 2];
    memset(out, 0, sizeof(out));
    int64_t got = pty_slave_read(n, out, sizeof(out));
    CHECK(got > 0);
    CHECK(got <= TTY_LINE_MAX);
    CHECK_EQ(out[0], 'x');
    CHECK_EQ(out[got - 1], '\n');
    done_pty(n);
}

TEST(pty, every_operation_refuses_a_number_that_names_no_pair) {
    char buf[8];
    CHECK_EQ(pty_tty(-1), NULL);
    CHECK_EQ(pty_tty(PTY_MAX), NULL);
    CHECK_EQ(pty_valid(0), 0);
    CHECK_EQ(pty_master_read(0, buf, sizeof(buf)), -1);
    CHECK_EQ(pty_master_write(0, "x", 1), -1);
    CHECK_EQ(pty_slave_read(0, buf, sizeof(buf)), -1);
    CHECK_EQ(pty_slave_write(0, "x", 1), -1);
    CHECK_EQ(pty_master_readable(0), 1);
    CHECK_EQ(pty_slave_readable(0), 1);
    pty_slave_closed(0);
    pty_master_closed(0);
    pty_slave_closed(-1);
    pty_master_closed(PTY_MAX + 3);
}

TEST(pty, kill_discards_the_line_being_edited_and_nothing_else) {
    int n = fresh_pty();
    char buf[64];

    pty_master_write(n, "keep\n", 5);
    pty_master_write(n, "throwaway", 9);
    pty_master_write(n, "\025", 1);
    pty_master_write(n, "\n", 1);

    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 6);
    CHECK(strcmp(buf, "keep\n\n") == 0);
    done_pty(n);
}

TEST(pty, erase_and_kill_on_an_empty_line_do_nothing_at_all) {
    int n = fresh_pty();
    char buf[64];

    pty_master_write(n, "\177\177\177", 3);
    CHECK_EQ(tty_out_readable(pty_tty(n)), 0);
    pty_master_write(n, "\025", 1);
    CHECK_EQ(tty_out_readable(pty_tty(n)), 0);

    pty_master_write(n, "ok\n", 3);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 3);
    CHECK(strcmp(buf, "ok\n") == 0);
    done_pty(n);
}

TEST(pty, with_echoe_off_an_erase_still_edits_but_does_not_unprint) {
    int n = fresh_pty();
    char buf[64];
    tty_t *t = pty_tty(n);
    REQUIRE(t != NULL);
    t->tio.c_lflag &= ~(tcflag_t)ECHOE;

    pty_master_write(n, "ab\177c\n", 5);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 3);
    CHECK(strcmp(buf, "ac\n") == 0);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_master_read(n, buf, sizeof(buf)), 5);
    CHECK(strcmp(buf, "abc\r\n") == 0);
    done_pty(n);
}

TEST(pty, a_master_with_bytes_queued_is_readable_before_anything_hangs_up) {
    int n = fresh_pty();
    CHECK_EQ(pty_master_readable(n), 0);
    pty_slave_write(n, "x", 1);
    CHECK_EQ(pty_master_readable(n), 1);
    char c = 0;
    CHECK_EQ(pty_master_read(n, &c, 1), 1);
    CHECK_EQ(c, 'x');
    CHECK_EQ(pty_master_readable(n), 0);
    done_pty(n);
}

TEST(pty, a_session_leaders_death_releases_only_its_own_terminal) {
    q13_boot();
    int a = pty_alloc();
    int b = pty_alloc();
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    pty_slave_opened(a);
    pty_slave_opened(b);

    tty_t *ta = pty_tty(a);
    tty_t *tb = pty_tty(b);
    REQUIRE(ta != NULL);
    REQUIRE(tb != NULL);
    ta->sid = 500;
    ta->fg_pgid = 500;
    tb->sid = 600;
    tb->fg_pgid = 600;

    task_t *victim = q13_spawn("in-session-500");
    REQUIRE(victim != NULL);
    victim->pgid = 500;

    pty_release_session(500);
    CHECK_EQ(ta->sid, 0);
    CHECK_EQ(ta->fg_pgid, 0);
    CHECK_EQ(tb->sid, 600);
    CHECK_EQ(tb->fg_pgid, 600);
    CHECK_EQ(victim->pending_signal, SIGHUP);

    victim->pending_signal = 0;
    pty_release_session(0);
    CHECK_EQ(tb->sid, 600);

    q13_kill(victim);
    done_pty(a);
    done_pty(b);
}

TEST(pty, the_console_is_not_a_pty_and_queues_no_output) {
    tty_init();
    tty_t *c = tty_console();
    tty_write(c, "hello\n", 6);
    CHECK_EQ(tty_out_readable(c), 0);
    tty_input_char(c, 'a');
    tty_input_char(c, '\n');
    CHECK_EQ(tty_out_readable(c), 0);
    CHECK_EQ(tty_readable(c), 2);
    char buf[8];
    CHECK_EQ(tty_read(c, buf, sizeof(buf)), 2);
}
