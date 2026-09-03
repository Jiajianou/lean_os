/* tests/test_pty.c - M85, second attempt
 *
 * The line discipline and the pty pair, off the machine.
 *
 * The boot self-test proves this path works when a real program opens
 * /dev/ptmx on a real disk under a real scheduler. What it cannot reach
 * is the arithmetic at the edges: a ring buffer that wraps, a queue that
 * fills, a pair that is freed while one end still names it, and the
 * eight-pair ceiling. Filling a 4096-byte output queue from inside a
 * booted machine means writing 4096 bytes and hoping; here it is a loop
 * that takes microseconds and an assertion about the exact byte that was
 * dropped.
 *
 * The signal assertions run against the REAL scheduler, which Q13 put in
 * this tier. That is worth stating because the first version of this
 * file did not: it used a fake that recorded "somebody asked to signal
 * process group 42" and asserted on the recording, which is a test of
 * the terminal's intention rather than of what happens. Now a real task
 * is spawned into a real process group and the assertion is on that
 * task's own pending signal - so a `tty_signal_foreground` that named
 * the wrong group, or a `sched_raise_signal_group` that matched the
 * wrong tasks, fails here.
 */
#include "check.h"
#include "fakes/fakes.h"

#include "dev/pty.h"
#include "dev/tty.h"
#include "sched/sched.h"
#include "signal.h" /* system_api/include/signal.h */

#include <string.h>

/* Every test starts from a machine with no pairs allocated. The table is
 * file-static in pty.c and there is no reset call, on purpose - a kernel
 * does not have one - so this closes what it opened instead, which also
 * makes "does closing both ends actually recycle the pair" a property
 * every test in this file depends on rather than one test asserts. */
/* Q13 put the real scheduler in this tier, so a terminal that signals a
 * process group can be pointed at a real one. sched_init is called once
 * for the whole binary - see tests/test_sched.c's q13_boot for why the
 * table is grown rather than reset between tests. */
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

/* ---- the discipline, on a terminal with no hardware ------------------- */

TEST(pty, a_line_is_delivered_whole_only_on_enter) {
    int n = fresh_pty();
    char buf[64];

    pty_master_write(n, "abX\177c", 5); /* 0177 is DEL - what backspace sends */
    /* Not one byte, and this is the property that makes it a terminal:
     * a pipe would have handed over "abX" the moment it was written. */
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
    /* "h", "i", then Enter echoed as CR LF - four bytes, and the slave's
     * three are a different queue entirely. Reading them off the same
     * buffer is the bug this separates: an implementation that echoed
     * into the input queue would deliver "hihi" to the program. */
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

    /* And with OPOST off it is exactly what was written, which is what
     * `stty -opost` has to mean or it means nothing. */
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

    /* Two tasks: one in the foreground group and one that is not. The
     * second is the whole point - a ^C that killed everything would pass
     * a test with only the first, and "the foreground group and not the
     * shell" is what this milestone's own bullet asks for. */
    task_t *fg = q13_spawn("fg");
    task_t *bg = q13_spawn("bg");
    REQUIRE(fg != NULL);
    REQUIRE(bg != NULL);
    fg->pgid = fg->id;
    bg->pgid = bg->id;
    t->sid = 7;
    t->fg_pgid = fg->id;

    pty_master_write(n, "\003", 1); /* ^C */
    CHECK_EQ(fg->pending_signal, SIGINT);
    CHECK_EQ(bg->pending_signal, 0);

    /* And ^Z is a stop rather than a death - the same character path,
     * the other disposition. */
    fg->pending_signal = 0;
    pty_master_write(n, "\032", 1);
    CHECK_EQ(fg->pending_stop, SIGTSTP);
    CHECK_EQ(bg->pending_stop, 0);

    /* And neither is a byte. A program reading this terminal sees
     * nothing at all, which is the difference between a signal and an
     * input character and the reason ^C works on a program that is not
     * reading. */
    char buf[8];
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 0);

    q13_kill(fg);
    q13_kill(bg);
    done_pty(n);
}

TEST(pty, an_unclaimed_terminal_signals_nobody) {
    /* fg_pgid 0 is what a pty looks like before anything calls
     * tcsetpgrp, and it is the state a ^C arrives in if a terminal
     * emulator sends one before its shell has started. Process group 0
     * is also task 0's group, so a terminal that raised on it anyway
     * would signal the kernel task - which is the failure this rules
     * out. */
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

/* ---- hangup, in both directions --------------------------------------- */

TEST(pty, closing_the_master_hangs_the_slave_up) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);

    char buf[8];
    CHECK_EQ(pty_slave_readable(n), 0); /* nothing typed: a read would wait */
    pty_master_closed(n);
    /* Ready, and reads zero. Both halves matter: ready-and-blocks is a
     * hang, and not-ready-forever is the same hang with a different
     * shape. */
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
    /* The bytes were committed before the hangup and are still the
     * program's to read. A hangup that discarded them would lose the
     * last line of every session. */
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 5);
    CHECK(strcmp(buf, "last\n") == 0);
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 0);
    pty_slave_closed(n);
}

TEST(pty, a_master_does_not_read_end_of_file_before_the_slave_opens) {
    /* The gap between opening /dev/ptmx and opening /dev/pts/<n>. A
     * master that reported end-of-file here would make every terminal
     * emulator on this machine exit during its own setup, and it is the
     * classic version of this bug. */
    int n = pty_alloc();
    REQUIRE(n >= 0);
    CHECK_EQ(pty_master_readable(n), 0);

    pty_slave_opened(n);
    CHECK_EQ(pty_master_readable(n), 0);
    pty_slave_closed(n);
    /* Now every slave that was open has closed, and that IS end of
     * file. */
    CHECK_EQ(pty_master_readable(n), 1);
    pty_master_closed(n);
    CHECK(!pty_valid(n));
}

TEST(pty, a_pair_survives_until_both_ends_close) {
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);
    pty_master_closed(n);
    CHECK(pty_valid(n)); /* the slave still names it */
    pty_slave_closed(n);
    CHECK(!pty_valid(n));

    /* And the other order. Both are real: a shell can exit before its
     * terminal emulator does, and a terminal emulator can be killed
     * while its shell is still running. */
    n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);
    pty_slave_closed(n);
    CHECK(pty_valid(n));
    pty_master_closed(n);
    CHECK(!pty_valid(n));
}

TEST(pty, two_slaves_on_one_pair_both_have_to_close) {
    /* What a fork does: the child inherits the slave descriptor, so the
     * pair has two open slave ends and the first close must not hang the
     * master up. */
    int n = pty_alloc();
    REQUIRE(n >= 0);
    pty_slave_opened(n);
    pty_slave_opened(n);
    pty_slave_closed(n);
    CHECK_EQ(pty_master_readable(n), 0); /* still one holder: not end of file */
    pty_slave_closed(n);
    CHECK_EQ(pty_master_readable(n), 1);
    pty_master_closed(n);
    CHECK(!pty_valid(n));
}

/* ---- the ceilings, which is what this tier is for --------------------- */

TEST(pty, the_ninth_pty_is_refused_and_the_table_recovers) {
    int held[PTY_MAX];
    for (int i = 0; i < PTY_MAX; i++) {
        held[i] = pty_alloc();
        CHECK(held[i] >= 0);
    }
    /* -1, not a grown table and not a reused pair. A ninth terminal
     * window is a refusal a program can report; a recycled pair would
     * be two programs sharing one terminal. */
    CHECK_EQ(pty_alloc(), -1);
    for (int i = 0; i < PTY_MAX; i++) {
        pty_master_closed(held[i]);
    }
    /* And the table is not permanently full afterwards, which is the
     * half a "does it refuse" test on its own would miss. */
    int again = pty_alloc();
    CHECK(again >= 0);
    pty_master_closed(again);
}

TEST(pty, the_output_queue_drops_rather_than_overwriting) {
    int n = fresh_pty();
    char one[1];
    /* A master that is not reading. Every real pty drops here; what
     * matters is WHICH bytes, and the answer has to be the newest ones -
     * dropping the oldest would hand the reader the tail of the output
     * with the beginning gone, which is unreadable in a different way. */
    for (int i = 0; i < TTY_OUTBUF * 2; i++) {
        char c = (char)('a' + (i % 26));
        pty_slave_write(n, &c, 1);
    }
    tty_t *t = pty_tty(n);
    REQUIRE(t != NULL);
    CHECK_EQ(tty_out_readable(t), TTY_OUTBUF - 1); /* one slot never fills: it is what distinguishes full from empty */
    /* The first byte still queued is the first byte written, so what was
     * lost is the end of the stream and not its beginning. */
    CHECK_EQ(pty_master_read(n, one, 1), 1);
    CHECK_EQ(one[0], 'a');
    done_pty(n);
}

TEST(pty, the_input_queue_wraps_without_losing_a_byte) {
    int n = fresh_pty();
    char buf[64];
    /* Enough lines to take the ring past TTY_INBUF several times, read
     * out as it goes. A modulo that was wrong by one shows up here as a
     * byte in the wrong place after the wrap and nowhere else. */
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
        /* And drain the echo, or the output ring fills and stops being
         * the thing under test. */
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
    /* Whatever the cap is, it holds, and the bytes that did arrive are
     * the ones typed FIRST plus the newline - a discipline that dropped
     * the front of the line would deliver a command missing its verb. */
    CHECK(got > 0);
    CHECK(got <= TTY_LINE_MAX);
    CHECK_EQ(out[0], 'x');
    CHECK_EQ(out[got - 1], '\n');
    done_pty(n);
}

/* ---- operations on a number that names nothing ------------------------ */

TEST(pty, every_operation_refuses_a_number_that_names_no_pair) {
    char buf[8];
    /* -1 rather than a crash, for every entry point. This is the
     * question a stale file descriptor asks, and the fd table is not the
     * only thing that can be wrong about a pty number - devfs decodes
     * one out of a path. */
    CHECK_EQ(pty_tty(-1), NULL);
    CHECK_EQ(pty_tty(PTY_MAX), NULL);
    CHECK_EQ(pty_valid(0), 0);
    CHECK_EQ(pty_master_read(0, buf, sizeof(buf)), -1);
    CHECK_EQ(pty_master_write(0, "x", 1), -1);
    CHECK_EQ(pty_slave_read(0, buf, sizeof(buf)), -1);
    CHECK_EQ(pty_slave_write(0, "x", 1), -1);
    /* Readable answers 1 for a dead pair, and that is deliberate: the
     * read then returns -1 immediately. Answering 0 would park the
     * caller on a descriptor that can never become ready. */
    CHECK_EQ(pty_master_readable(0), 1);
    CHECK_EQ(pty_slave_readable(0), 1);
    /* And the close paths tolerate it, because they run from teardown
     * where a failure has nobody to report to. */
    pty_slave_closed(0);
    pty_master_closed(0);
    pty_slave_closed(-1);
    pty_master_closed(PTY_MAX + 3);
}

/* ---- the line-editing branches `make mutate` found uncovered -----------
 *
 * Each of these exists because a mutation survived: the harness broke
 * the branch on purpose, every test above still passed, and that is the
 * definition of coverage without an assertion. The console self-test on
 * the machine covers some of them; a boot marker is not a substitute for
 * a check that runs in microseconds, and the pty is where these are
 * cheapest to drive.
 */

TEST(pty, kill_discards_the_line_being_edited_and_nothing_else) {
    int n = fresh_pty();
    char buf[64];

    /* ^U after a committed line must take the half-typed one and leave
     * the committed one alone. A discipline that flushed everything
     * loses a command the user already pressed Enter on. */
    pty_master_write(n, "keep\n", 5);
    pty_master_write(n, "throwaway", 9);
    pty_master_write(n, "\025", 1); /* ^U */
    pty_master_write(n, "\n", 1);

    memset(buf, 0, sizeof(buf));
    CHECK_EQ(pty_slave_read(n, buf, sizeof(buf)), 6);
    CHECK(strcmp(buf, "keep\n\n") == 0);
    done_pty(n);
}

TEST(pty, erase_and_kill_on_an_empty_line_do_nothing_at_all) {
    int n = fresh_pty();
    char buf[64];

    /* Backspace at a bare prompt is what a person does constantly. It
     * must not underflow the line length, and it must not echo the
     * backspace-space-backspace that would walk the cursor back over the
     * prompt itself - which is the visible bug, and the one that made
     * this worth a test rather than a bounds check. */
    pty_master_write(n, "\177\177\177", 3);
    CHECK_EQ(tty_out_readable(pty_tty(n)), 0);
    pty_master_write(n, "\025", 1); /* ^U on nothing */
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
    /* The line is still edited - ECHOE is about the display, not about
     * the buffer - and the echo is 'a','b','c' plus CR LF with no
     * backspace sequence in it. A discipline that tied the two together
     * would either stop erasing or start printing. */
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
    /* The other arm of pty_master_readable: not end-of-file, but actual
     * bytes. Both arms answer 1 and only one of them is followed by a
     * read that returns something, so a test on the answer alone passes
     * against a function that always says yes. */
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

    /* Session 500 ends. Its terminal is handed back and its foreground
     * job gets a SIGHUP; session 600's terminal is untouched. A release
     * that walked every pty and freed all of them would pass a test with
     * one terminal in it, which is why there are two. */
    pty_release_session(500);
    CHECK_EQ(ta->sid, 0);
    CHECK_EQ(ta->fg_pgid, 0);
    CHECK_EQ(tb->sid, 600);
    CHECK_EQ(tb->fg_pgid, 600);
    CHECK_EQ(victim->pending_signal, SIGHUP);

    /* And a session that owns nothing releases nothing rather than
     * everything - session 0 is what an unclaimed terminal reports. */
    victim->pending_signal = 0;
    pty_release_session(0);
    CHECK_EQ(tb->sid, 600);

    q13_kill(victim);
    done_pty(a);
    done_pty(b);
}

/* ---- the console, which shares every line of this code ---------------- */

TEST(pty, the_console_is_not_a_pty_and_queues_no_output) {
    /* The same discipline with the other sink. If this ever starts
     * queueing, the console has silently become a pty that nobody reads
     * and every byte the machine printed is sitting in a 4 KiB buffer. */
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
