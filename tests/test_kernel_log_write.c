/* One write() to the console, through the real kernel_log.c.

   On eight cores the battery failed on a marker that had been printed: its
   '[' was spliced into another program's line, because sys_write handed the
   log one character at a time and the log took and dropped its lock around
   each one. What a second processor can do is write whenever the lock is
   free - so the fake spinlock's release hook plays that processor here, and
   writes a '#' every time any lock is let go of. A write that is one piece
   has its '#'s only at its edges; the per-character loop it replaced has one
   between every two characters.

   The real file is compiled here with every name it exports renamed, so it
   does not collide with tests/fakes/fake_klog.c, which every other host test
   logs through. Its serial port is this file's buffer. */

#include "check.h"
#include "fakes/fakes.h"

#include <stdio.h>
#include <string.h>

#include "drivers/font8x16.h"

void fake_framebuffer_reset(uint32_t w, uint32_t h);
unsigned long long fake_framebuffer_writes(void);

#define kernel_log_init             klog_under_test_init
#define kernel_log_use_console       klog_under_test_use_console
#define kernel_log_release_console   klog_under_test_release_console
#define kernel_log_begin             klog_under_test_begin
#define kernel_log_begin_report      klog_under_test_begin_report
#define kernel_log_end               klog_under_test_end
#define kernel_log_enter_panic       klog_under_test_enter_panic
#define kernel_log_putc              klog_under_test_putc
#define kernel_log_puts              klog_under_test_puts
#define kernel_log_write             klog_under_test_write
#define kernel_log_write_cut         klog_under_test_write_cut
#define kernel_log_write_from        klog_under_test_write_from
#define kernel_log_put_dec_pad       klog_under_test_put_dec_pad
#define kernel_log_put_dec           klog_under_test_put_dec
#define kernel_log_put_hex32         klog_under_test_put_hex32
#define kernel_log_put_hex64         klog_under_test_put_hex64
#define kernel_log_log               klog_under_test_log
#define kernel_log_log_hex64         klog_under_test_log_hex64
#define kernel_log_read              klog_under_test_read
#define kernel_log_written_total     klog_under_test_written_total
#define kernel_log_line_time         klog_under_test_line_time
#define kernel_log_irq_off_stats     klog_under_test_irq_off_stats

#include "../kernel/drivers/kernel_log.c"

void serial_init(void) {}

/* The serial port: what reached it, in order. */
static char wire[16384];
static size_t wire_bytes;
static void wire_put(char c) {
    if (wire_bytes < sizeof(wire) - 1) {
        wire[wire_bytes++] = c;
        wire[wire_bytes] = '\0';
    }
}
static void wire_reset(void) {
    wire_bytes = 0;
    wire[0] = '\0';
}

/* The NMI that stops every other processor. What the drain's owner was when
   it was sent says whether the panic halted that processor before it took
   its claim, or after. */
static int halts_sent;
static int owner_when_halted = -2;
void smp_halt_other_cpus(void) {
    halts_sent++;
    owner_when_halted = kernel_log_drain_owner;
}

static int interrupts_are_on(void) {
    uint64_t was = fake_irq_save_disable();
    fake_irq_restore(was);
    return was != 0;
}

/* How many times in a row the port answers that it is busy, and how many of
   those answers it gave with interrupts off - which is a processor spinning
   on a slow port with the machine deaf, the thing M225 took away. */
static int port_busy_answers;
static int port_busy_with_interrupts_off;
int serial_tx_room(void) {
    if (port_busy_answers > 0) {
        port_busy_answers--;
        if (!interrupts_are_on()) {
            port_busy_with_interrupts_off++;
        }
        return 0;
    }
    return 16;
}
/* Called after every character the port takes: a test can play another
   processor at that exact point - in the middle of a batch. */
static void (*after_port_char)(void);
void serial_tx_put(char c) {
    wire_put(c);
    if (after_port_char) {
        after_port_char();
    }
}
void serial_putc(char c) { wire_put(c); }

/* Wherever this processor would spin waiting for another one, the test
   plays the other one: what it does is up to the test. */
void kernel_log_test_relax(void);
static void (*relax_hook)(void);
static int relaxes;
void kernel_log_test_relax(void) {
    relaxes++;
    if (relax_hook) {
        relax_hook();
    }
}

/* Everything the log received since `from`, as a string. */
static char log_text[8192];
static const char *log_since(uint64_t from) {
    uint64_t next = 0;
    size_t n = kernel_log_read(from, log_text, sizeof(log_text) - 1, &next);
    log_text[n] = '\0';
    return log_text;
}

static int other_processor_writes;
static void other_processor_writes_a_byte(void) {
    other_processor_writes++;
    kernel_log_putc('#');
}

static void start(void) {
    other_processor_writes = 0;
    fake_spinlock_on_release(other_processor_writes_a_byte);
}

/* An interrupt that is always pending: it is taken every time interrupts
   come back on, and it notes how far the serial port had got. The longest
   stretch of the wire between two of them is the most the log sent with
   interrupts off. */
static size_t interrupt_at_wire[4096];
static int interrupts_taken;
static void pending_interrupt(void) {
    if (interrupts_taken < (int)(sizeof(interrupt_at_wire) / sizeof(interrupt_at_wire[0]))) {
        interrupt_at_wire[interrupts_taken++] = wire_bytes;
    }
    fake_arch_raise_interrupt(pending_interrupt);
}

static size_t longest_stretch_without_an_interrupt(void) {
    size_t longest = 0;
    size_t last = 0;
    for (int i = 0; i < interrupts_taken; i++) {
        if (interrupt_at_wire[i] - last > longest) {
            longest = interrupt_at_wire[i] - last;
        }
        last = interrupt_at_wire[i];
    }
    if (wire_bytes - last > longest) {
        longest = wire_bytes - last;
    }
    return longest;
}

TEST(kernel_log_write, a_line_is_one_piece_however_often_another_processor_writes) {
    uint64_t from = kernel_log_written_total();
    static const char line[] = "[m127] task_manager rendered 168862 pixels\n";
    start();
    CHECK_EQ(kernel_log_write_from(line, sizeof(line) - 1, 0), (long)(sizeof(line) - 1));
    fake_spinlock_on_release(0);
    const char *got = log_since(from);
    CHECK_MSG(strstr(got, line) != NULL, "the line was torn: \"%s\"", got);
    CHECK(other_processor_writes > 0);
}

static int copies;
static int copies_under_a_lock;
static int fail_copy_number;
static int copy_counting_locks(char *to, const char *from, size_t length) {
    copies++;
    /* A user buffer can fault, and a fault with interrupts off and the
       log's lock held is every other processor's next log line spinning
       forever. The copy has to happen with neither lock held. */
    if (kernel_log_lock.locked || kernel_log_message_lock.locked) {
        copies_under_a_lock++;
    }
    if (copies == fail_copy_number) {
        return -1;
    }
    memcpy(to, from, length);
    return 0;
}

static void reset_copies(int fail_at) {
    copies = 0;
    copies_under_a_lock = 0;
    fail_copy_number = fail_at;
}

TEST(kernel_log_write, a_long_write_is_cut_after_newlines_and_no_line_is_torn) {
    /* 24 lines of 40 bytes: 960 bytes, four pieces' worth. Wherever the
       other processor's '#' landed, it is between lines. */
    char text[24 * 40 + 1];
    for (int l = 0; l < 24; l++) {
        char *at = text + l * 40;
        memset(at, 'a' + l, 39);
        at[0] = '[';
        at[39] = '\n';
    }
    text[sizeof(text) - 1] = '\0';
    uint64_t from = kernel_log_written_total();
    reset_copies(0);
    start();
    CHECK_EQ(kernel_log_write_from(text, sizeof(text) - 1, copy_counting_locks), (long)(sizeof(text) - 1));
    fake_spinlock_on_release(0);
    CHECK_EQ(copies_under_a_lock, 0);
    CHECK(copies >= 4);
    const char *got = log_since(from);
    for (int l = 0; l < 24; l++) {
        char line[41];
        memcpy(line, text + l * 40, 40);
        line[40] = '\0';
        CHECK_MSG(strstr(got, line) != NULL, "line %d was torn: \"%s\"", l, got);
    }
    /* And nothing was written twice: the part of a piece after its last
       newline is copied again as the start of the next, not emitted. */
    size_t letters = 0;
    for (const char *p = got; *p; p++) {
        if (*p != '#') {
            letters++;
        }
    }
    CHECK_EQ(letters, sizeof(text) - 1);
}

TEST(kernel_log_write, a_line_longer_than_a_piece_is_split_at_the_piece) {
    /* The documented limit: no newline to cut at, so a piece is full. */
    char text[KERNEL_LOG_WRITE_CHUNK * 2 + 10];
    memset(text, 'x', sizeof(text));
    uint64_t from = kernel_log_written_total();
    start();
    CHECK_EQ(kernel_log_write_from(text, sizeof(text), 0), (long)sizeof(text));
    fake_spinlock_on_release(0);
    const char *got = log_since(from);
    size_t run = strspn(got, "x");
    CHECK_EQ(run, KERNEL_LOG_WRITE_CHUNK);
}

TEST(kernel_log_write, a_copy_that_fails_reports_what_was_written_before_it) {
    char text[KERNEL_LOG_WRITE_CHUNK * 3];
    memset(text, 'y', sizeof(text));
    reset_copies(1);
    CHECK_EQ(kernel_log_write_from(text, sizeof(text), copy_counting_locks), -1);
    reset_copies(2);
    CHECK_EQ(kernel_log_write_from(text, sizeof(text), copy_counting_locks), (long)KERNEL_LOG_WRITE_CHUNK);
    reset_copies(3);
    CHECK_EQ(kernel_log_write_from(text, sizeof(text), copy_counting_locks), (long)(2 * KERNEL_LOG_WRITE_CHUNK));
}

TEST(kernel_log_write, where_a_piece_is_cut) {
    CHECK_EQ(kernel_log_write_cut("ab\ncd\nef", 8, 1), 6);
    CHECK_EQ(kernel_log_write_cut("ab\ncd\nef", 8, 0), 8);
    CHECK_EQ(kernel_log_write_cut("abcdef", 6, 1), 6);
    CHECK_EQ(kernel_log_write_cut("abcde\n", 6, 1), 6);
    CHECK_EQ(kernel_log_write_cut("\nabcde", 6, 1), 1);
}

TEST(kernel_log_write, a_panicking_machine_writes_without_the_lock) {
    /* A panic can be taken by the processor holding the log's lock, and
       the panic path's last words must not wait for it. */
    uint64_t from = kernel_log_written_total();
    kernel_log_lock.locked = 1;
    kernel_log_panicking = 1;
    kernel_log_write("last words\n", 11);
    kernel_log_panicking = 0;
    kernel_log_lock.locked = 0;
    CHECK_STREQ(log_since(from), "last words\n");
}

/* M225, (1). A write's bytes went to the serial port a character at a time
   with the log's lock held and interrupts off for the whole write - 256
   characters of a 38400-baud port is ~67 ms of a machine that cannot take a
   timer tick. Now the ring takes the write in one hold and the devices are
   fed from it KERNEL_LOG_DRAIN_BATCH characters per interrupts-off section.
   An interrupt left pending is taken at every point interrupts come back
   on; between two of them the port may get one batch at most. */
TEST(kernel_log_write, the_devices_are_fed_a_batch_at_a_time_with_interrupts_on_between) {
    char text[200];
    for (size_t i = 0; i < sizeof(text); i++) {
        text[i] = (char)('a' + i % 26);
    }
    text[sizeof(text) - 1] = '\n';
    uint64_t from = kernel_log_written_total();
    wire_reset();
    kernel_log_irq_off_stats(0, 1);
    interrupts_taken = 0;
    fake_arch_raise_interrupt(pending_interrupt);
    CHECK_EQ(kernel_log_write_from(text, sizeof(text), 0), (long)sizeof(text));
    fake_arch_raise_interrupt(0);
    /* All of it reached the port, in order, and the ring holds the same. */
    CHECK_EQ(wire_bytes, sizeof(text));
    CHECK(memcmp(wire, text, sizeof(text)) == 0);
    CHECK(memcmp(log_since(from), text, sizeof(text)) == 0);
    size_t longest = longest_stretch_without_an_interrupt();
    CHECK_MSG(longest <= KERNEL_LOG_DRAIN_BATCH,
              "%zu characters went to the port with interrupts off in one stretch", longest);
    kernel_log_irq_off_t windows;
    kernel_log_irq_off_stats(&windows, 0);
    CHECK(windows.max_device_chars <= KERNEL_LOG_DRAIN_BATCH);
    CHECK(windows.max_device_chars > 0);
    CHECK(windows.windows >= sizeof(text) / KERNEL_LOG_DRAIN_BATCH);
}

/* A port still sending its FIFO is waited for with interrupts as the caller
   had them - never by a processor that has turned them off. */
TEST(kernel_log_write, a_busy_port_is_waited_for_with_interrupts_on) {
    wire_reset();
    port_busy_answers = 40;
    port_busy_with_interrupts_off = 0;
    relaxes = 0;
    kernel_log_write("[m127] task_manager rendered\n", 29);
    CHECK_EQ(port_busy_answers, 0);
    /* The owner of the drain asks the port once, inside its section; told
       it is busy, it lets go and the other 39 answers are waited for with
       interrupts on. */
    CHECK(port_busy_with_interrupts_off <= 1);
    CHECK(relaxes >= 39);
    CHECK_STREQ(wire, "[m127] task_manager rendered\n");
}

/* Another processor owns the drain: this one waits for it - holding no lock
   - and its bytes still come out after everything before them. */
static void other_processor_finishes_its_batch(void) {
    CHECK_EQ(kernel_log_lock.locked, 0u);
    CHECK_EQ(kernel_log_message_lock.locked, 0u);
    if (relaxes >= 3) {
        /* It sends the batch it had claimed and lets go. */
        kernel_log_drain_owner = -1;
    }
}

TEST(kernel_log_write, a_drain_another_processor_owns_is_waited_for_and_order_kept) {
    wire_reset();
    relaxes = 0;
    relax_hook = other_processor_finishes_its_batch;
    kernel_log_drain_owner = 1;
    kernel_log_write("second\n", 7);
    relax_hook = 0;
    CHECK_EQ(kernel_log_drain_owner, -1);
    CHECK(relaxes >= 3);
    CHECK_STREQ(wire, "second\n");
}

/* An NMI or a fault taken in the middle of this processor's own batch cannot
   wait for the batch to finish - it is what the batch is waiting for. It
   leaves its bytes in the ring, and the next drain sends them, in order,
   ahead of what follows. */
static void the_test_would_hang(void) {
    if (relaxes > 1000) {
        kernel_log_drain_owner = -1;
    }
}

TEST(kernel_log_write, an_interrupt_inside_this_processors_own_batch_does_not_wait_for_itself) {
    wire_reset();
    relaxes = 0;
    relax_hook = the_test_would_hang;
    kernel_log_drain_owner = 0;
    kernel_log_puts("[nmi] where this core is\n");
    CHECK_EQ(relaxes, 0);
    CHECK_STREQ(wire, "");
    kernel_log_drain_owner = -1;
    kernel_log_puts("next\n");
    relax_hook = 0;
    CHECK_STREQ(wire, "[nmi] where this core is\nnext\n");
}

/* A plain begin/end message (a [perf] line) goes into the ring call by call
   and to the devices at its end, so nothing is sent while the message lock
   is held. A crash report is the exception - see below. */
TEST(kernel_log_write, a_message_reaches_the_devices_at_its_end) {
    wire_reset();
    uint64_t message = kernel_log_begin();
    kernel_log_puts("[perf] ");
    kernel_log_puts("anim_frame_gap_ms ");
    kernel_log_put_dec(19);
    kernel_log_putc('\n');
    CHECK_STREQ(wire, "");
    kernel_log_end(message);
    CHECK_STREQ(wire, "[perf] anim_frame_gap_ms 19\n");
}

/* A panic does not wait for a drain somebody else owns - that processor may
   never run again - and sends what was still waiting in the ring first. */
TEST(kernel_log_write, a_panic_sends_what_was_waiting_and_then_its_own_words) {
    wire_reset();
    kernel_log_drain_owner = 0;
    kernel_log_puts("still in the ring\n");
    CHECK_STREQ(wire, "");
    kernel_log_drain_owner = 1;
    kernel_log_panicking = 1;
    kernel_log_write("last words\n", 11);
    kernel_log_puts("*** KERNEL PANIC\n");
    kernel_log_panicking = 0;
    kernel_log_drain_owner = -1;
    CHECK_STREQ(wire, "still in the ring\nlast words\n*** KERNEL PANIC\n");
}

/* M225 (log-crash-path), (1). The unhandled-exception path is begin, a
   heading, dump_regs, end, panic - and since 47c3b17 a message's bytes
   waited in the ring for its end. dump_regs runs on whatever stack the
   fault left; a second fault there, or a triple fault, resets the machine
   before kernel_log_end, and the report was then on neither the serial line
   nor the screen. Each check below is a point where the machine may reset:
   what was written must already be on the wire. */
TEST(kernel_log_write, a_crash_report_reaches_the_devices_as_it_is_written) {
    wire_reset();
    uint64_t message = kernel_log_begin_report();
    kernel_log_puts("\n*** UNHANDLED CPU EXCEPTION: Page fault ***\n");
    CHECK_STREQ(wire, "\n*** UNHANDLED CPU EXCEPTION: Page fault ***\n");
    kernel_log_puts("  vector=0x");
    kernel_log_put_hex64(14);
    CHECK_STREQ(wire, "\n*** UNHANDLED CPU EXCEPTION: Page fault ***\n  vector=0x000000000000000E");
    kernel_log_end(message);
    CHECK_STREQ(wire, "\n*** UNHANDLED CPU EXCEPTION: Page fault ***\n  vector=0x000000000000000E");
    CHECK_EQ(kernel_log_message_owner, -1);
    CHECK_EQ(kernel_log_message_report_depth, 0);
}

/* A fault taken while this processor was building a [perf] line: the
   report goes out as written, and the half line before it with it - it is
   ahead of the report in the ring. Once the report ends, the plain message
   around it is plain again and waits for its own end. */
TEST(kernel_log_write, a_report_inside_a_plain_message_sends_both_and_then_waits_again) {
    wire_reset();
    uint64_t outer = kernel_log_begin();
    kernel_log_puts("[perf] half a line ");
    CHECK_STREQ(wire, "");
    uint64_t report = kernel_log_begin_report();
    kernel_log_puts("*** fault\n");
    CHECK_STREQ(wire, "[perf] half a line *** fault\n");
    kernel_log_end(report);
    kernel_log_puts("rest\n");
    CHECK_STREQ(wire, "[perf] half a line *** fault\n");
    kernel_log_end(outer);
    CHECK_STREQ(wire, "[perf] half a line *** fault\nrest\n");
}

/* A report drains holding the message lock with interrupts off, so it must
   not wait for ever on another processor's batch: that processor may be in
   an NMI or fault of its own, taken inside the batch, whose report spins on
   the very message lock this one holds - or it may be the hang the report
   is about. It waits while that batch makes progress, gives up once it has
   made none for the patience, and from then on the report waits for nobody,
   its END included: the unhandled-exception path is begin, puts, dump_regs,
   end, panic, with interrupts off, and an end that waited without limit
   would never reach the panic - which is what sends the rest, taking the
   drain over. Processor 1 never comes back here; the test's hook only
   frees it when this processor has spun far past any patience, which is
   the hang, reported. */
static int spun_for_ever;
static void processor_1_never_comes_back(void) {
    if (relaxes > 50 * (int)KERNEL_LOG_PATIENCE_SPINS) {
        spun_for_ever = 1;
        kernel_log_drain_owner = -1;
    }
}

TEST(kernel_log_write, a_report_does_not_wait_for_ever_on_a_batch_that_makes_no_progress) {
    wire_reset();
    kernel_log_drain_owner = 1;
    relaxes = 0;
    spun_for_ever = 0;
    halts_sent = 0;
    relax_hook = processor_1_never_comes_back;
    uint64_t message = kernel_log_begin_report();
    kernel_log_puts("*** UNHANDLED CPU EXCEPTION: NMI ***\n");
    int first = relaxes;
    CHECK_EQ(first, (int)KERNEL_LOG_PATIENCE_SPINS);
    kernel_log_puts("  rip=0x1\n");
    CHECK_EQ(relaxes, first);
    CHECK_STREQ(wire, "");
    kernel_log_end(message);
    CHECK_EQ(spun_for_ever, 0);
    CHECK_EQ(relaxes, first);
    CHECK_EQ(kernel_log_message_owner, -1);
    CHECK_EQ(kernel_log_message_report_stalled, 0);
    CHECK_EQ(kernel_log_drain_owner, 1);
    /* panic(): its words, after the report it takes the drain over to
       send - halting processor 1 first. */
    kernel_log_panicking = 1;
    kernel_log_puts("*** KERNEL PANIC\n");
    kernel_log_panicking = 0;
    relax_hook = 0;
    CHECK_EQ(spun_for_ever, 0);
    CHECK_STREQ(wire, "*** UNHANDLED CPU EXCEPTION: NMI ***\n  rip=0x1\n*** KERNEL PANIC\n");
    CHECK_EQ(halts_sent, 1);
    CHECK_EQ(kernel_log_drain_owner, -1);
}

/* The other half: every write of the report got through, and THEN another
   processor's batch stopped making progress with bytes of its own still in
   the ring. The report's end, on its way to panic() with interrupts off,
   gives up on it after the patience as its writes would have - it was an
   end with no limit, and the probe that found it spun past 200,000 waits. */
TEST(kernel_log_write, a_reports_end_does_not_wait_for_ever_either) {
    wire_reset();
    relaxes = 0;
    spun_for_ever = 0;
    uint64_t message = kernel_log_begin_report();
    kernel_log_puts("*** UNHANDLED CPU EXCEPTION: Page fault ***\n");
    CHECK_STREQ(wire, "*** UNHANDLED CPU EXCEPTION: Page fault ***\n");
    CHECK_EQ(relaxes, 0);
    /* An NMI on this processor appends without draining (it "owns" the
       drain), and then processor 1 claims a batch and stops. */
    kernel_log_drain_owner = 0;
    kernel_log_puts("[nmi] owed\n");
    kernel_log_drain_owner = 1;
    relax_hook = processor_1_never_comes_back;
    kernel_log_end(message);
    relax_hook = 0;
    CHECK_EQ(spun_for_ever, 0);
    CHECK_EQ(relaxes, (int)KERNEL_LOG_PATIENCE_SPINS);
    CHECK_EQ(kernel_log_message_owner, -1);
    CHECK_STREQ(wire, "*** UNHANDLED CPU EXCEPTION: Page fault ***\n");
    kernel_log_drain_owner = -1;
    kernel_log_puts("next\n");
    CHECK_STREQ(wire, "*** UNHANDLED CPU EXCEPTION: Page fault ***\n[nmi] owed\nnext\n");
}

/* (2). A panic used to flush with no claim on the drain. Processor 1 is
   half way through a batch - it has sent "01234" of "0123456789\n" and not
   yet said so - when this processor panics. The panic waits for that batch
   (processor 1 is making progress), and so nothing comes out twice. */
static int other_batch_relaxes;
static uint64_t other_batch_end;
static void other_processor_ends_its_batch(void) {
    if (++other_batch_relaxes == 3) {
        kernel_log_drain_release(1, other_batch_end);
    }
}

/* Bytes into the ring that no drain sends: this processor "owns" the drain,
   as an NMI inside its own batch does. */
static uint64_t leave_in_the_ring(const char *s) {
    uint64_t from = kernel_log_written_total();
    kernel_log_drain_owner = 0;
    kernel_log_puts(s);
    kernel_log_drain_owner = -1;
    return from;
}

TEST(kernel_log_write, a_panic_waits_for_the_batch_another_processor_is_sending) {
    wire_reset();
    uint64_t from = leave_in_the_ring("0123456789\n");
    CHECK_STREQ(wire, "");
    kernel_log_drain_owner = 1;
    wire_put('0');
    wire_put('1');
    wire_put('2');
    wire_put('3');
    wire_put('4');
    other_batch_relaxes = 0;
    other_batch_end = from + 5;
    relax_hook = other_processor_ends_its_batch;
    kernel_log_panicking = 1;
    kernel_log_write("last words\n", 11);
    kernel_log_panicking = 0;
    relax_hook = 0;
    CHECK(other_batch_relaxes >= 3);
    CHECK_STREQ(wire, "0123456789\nlast words\n");
    CHECK_EQ(kernel_log_drain_owner, -1);
}

/* A processor the panic halted in the middle of its batch never lets go of
   the drain: the panic does not wait for it for ever, and takes the drain
   over once it has made no progress for the patience. If that processor
   did come back and end its batch, it would end it where IT started -
   behind the panic - and that must not move the devices back, or every
   later write sends the panic's words again. */
TEST(kernel_log_write, a_panic_takes_over_from_a_batch_that_never_ends_and_the_devices_never_go_back) {
    wire_reset();
    kernel_log_irq_off_t stats;
    kernel_log_irq_off_stats(&stats, 0);
    uint64_t takeovers = stats.panic_takeovers;
    uint64_t from = leave_in_the_ring("still in the ring\n");
    kernel_log_drain_owner = 1;
    relaxes = 0;
    halts_sent = 0;
    owner_when_halted = -2;
    kernel_log_panicking = 1;
    kernel_log_write("last words\n", 11);
    kernel_log_panicking = 0;
    CHECK(relaxes >= (int)KERNEL_LOG_PATIENCE_SPINS);
    /* Processor 1 was halted while it still held the claim: one that was
       only slow cannot come back mid-batch beside the panic's flush. */
    CHECK_EQ(halts_sent, 1);
    CHECK_EQ(owner_when_halted, 1);
    CHECK_STREQ(wire, "still in the ring\nlast words\n");
    kernel_log_irq_off_stats(&stats, 0);
    CHECK_EQ(stats.panic_takeovers, takeovers + 1);
    /* Processor 1 wakes and ends the batch it had started at `from`. */
    kernel_log_drain_release(1, from + 4);
    CHECK_EQ(kernel_log_drain_owner, -1);
    kernel_log_puts("after\n");
    CHECK_STREQ(wire, "still in the ring\nlast words\nafter\n");
}

/* A panic that took the claim over (from an owner it halted - but one
   inside an NMI handler takes the halt only on its way out) must not then
   share the console with the rest of that owner's batch: the cursor, the
   grid and the repaint position are one processor's at a time. The owner
   looks at the claim before every character and stops at the next one. */
static size_t wire_when_the_claim_went;
static int port_chars_seen;
static void a_panic_takes_the_claim_after_three(void) {
    if (++port_chars_seen == 3) {
        kernel_log_drain_owner = 1;
        wire_when_the_claim_went = wire_bytes;
    }
}
static size_t wire_at_first_wait;
static void the_panic_finishes(void) {
    if (wire_at_first_wait == 0) {
        wire_at_first_wait = wire_bytes;
    }
    kernel_log_drain_owner = -1;
}

TEST(kernel_log_write, a_batch_the_panic_took_over_stops_at_the_next_character) {
    wire_reset();
    port_chars_seen = 0;
    wire_at_first_wait = 0;
    after_port_char = a_panic_takes_the_claim_after_three;
    relax_hook = the_panic_finishes;
    kernel_log_write("0123456789abcdef\n", 17);
    after_port_char = 0;
    relax_hook = 0;
    CHECK_EQ(wire_when_the_claim_went, (size_t)3);
    CHECK_MSG(wire_at_first_wait == 3,
              "the batch sent %zu characters after its claim was taken", wire_at_first_wait - 3);
    CHECK_EQ(kernel_log_drain_owner, -1);
}

/* (4). Through the log onto the ThinkPad's 3840x2400 console: an interrupt
   always pending is taken whenever interrupts come back on, and no stretch
   between two of them paints more than KERNEL_LOG_SECTION_CELLS cells - a
   scroll is repainted a piece per section. And a write still returns with
   its bytes on the screen: no repaint is left owed when it does. */
static unsigned long long pixels_at_last_interrupt;
static unsigned long long longest_painting;
static int painted_interrupts;
static void pending_interrupt_counting_pixels(void) {
    unsigned long long now = fake_framebuffer_writes();
    if (now - pixels_at_last_interrupt > longest_painting) {
        longest_painting = now - pixels_at_last_interrupt;
    }
    pixels_at_last_interrupt = now;
    painted_interrupts++;
    fake_arch_raise_interrupt(pending_interrupt_counting_pixels);
}

TEST(kernel_log_write, on_the_4k_panel_no_section_paints_more_than_its_cells) {
    fake_framebuffer_reset(3840, 2400);
    console_init();
    use_console = 1;
    wire_reset();
    pixels_at_last_interrupt = fake_framebuffer_writes();
    longest_painting = 0;
    painted_interrupts = 0;
    int owed_after_a_write = 0;
    fake_arch_raise_interrupt(pending_interrupt_counting_pixels);
    char line[80];
    for (int i = 0; i < 400; i++) {
        int n = snprintf(line, sizeof(line), "[boot] line %d of a boot log long enough to scroll\n", i);
        kernel_log_write(line, (size_t)n);
        owed_after_a_write += console_repaint_pending();
        wire_reset();
    }
    fake_arch_raise_interrupt(0);
    use_console = 0;
    CHECK_EQ(owed_after_a_write, 0);
    CHECK(painted_interrupts > 400);
    CHECK_MSG(longest_painting <= (unsigned long long)KERNEL_LOG_SECTION_CELLS * FONT_WIDTH * FONT_HEIGHT,
              "%llu pixels painted with interrupts off in one stretch", longest_painting);
    kernel_log_irq_off_t stats;
    kernel_log_irq_off_stats(&stats, 0);
    CHECK(stats.boot_max_console_cells > 0);
    CHECK(stats.boot_max_console_cells <= KERNEL_LOG_SECTION_CELLS);
    CHECK(stats.boot_repaint_sections > 0);
}
