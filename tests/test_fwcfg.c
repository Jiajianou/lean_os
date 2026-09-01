/* tests/test_fwcfg.c - Q11
 *
 * The 148 lines that decide whether any of the boot self-tests run.
 *
 * This file is the one Q1 added and the one Q11's audit called out for
 * having no test of its own, and the reason it matters more than its size
 * suggests is the shape of its failure. If boot_selftests_enabled() were
 * to return 0 when it should return 1 - a directory walk that stops one
 * entry early, a name comparison that matches a prefix, an endianness
 * mistake in a field read - then tools/qemu-serial-test.sh would boot a
 * machine that runs no self-tests, find no markers, and... fail loudly,
 * because the markers are required. That is the good case.
 *
 * The bad case is the other direction of the same bug reaching the
 * *format* of the answer rather than its value. fw_cfg's file directory
 * mixes endiannesses - the selector port takes a little-endian value on
 * x86 while every integer inside the directory is big-endian - and a
 * kernel that reads a count with the wrong one walks a loop bounded by a
 * number from outside the machine. There is a ceiling in the code for
 * exactly that reason and nothing has ever tested it.
 *
 * The device itself is two I/O ports, so the fake here is a scripted
 * answer to a sequence of port reads: tests/fakes/arch/x86_64/io.h is
 * already a shadow, and this file supplies the bytes behind it. */
#include "check.h"
#include "fakes/fakes.h"
#include "dev/fwcfg.h"

#include <stdint.h>
#include <string.h>

#define FWCFG_SIGNATURE 0x0000
#define FWCFG_FILE_DIR  0x0019

/* Builds a fw_cfg file directory exactly as QEMU lays one out: a
 * big-endian count, then 64-byte entries of {be32 size, be16 selector,
 * be16 reserved, char name[56]}. Written out by hand rather than with a
 * struct, because the endianness is the thing under test and a packed
 * struct would hide it. */
static void dir_begin(uint32_t count) {
    uint8_t hdr[4] = {(uint8_t)(count >> 24), (uint8_t)(count >> 16),
                      (uint8_t)(count >> 8), (uint8_t)count};
    fake_fwcfg_set_item(FWCFG_FILE_DIR, hdr, sizeof(hdr));
}

static void dir_add(const char *name, uint32_t size, uint16_t selector) {
    uint8_t entry[64];
    memset(entry, 0, sizeof(entry));
    entry[0] = (uint8_t)(size >> 24);
    entry[1] = (uint8_t)(size >> 16);
    entry[2] = (uint8_t)(size >> 8);
    entry[3] = (uint8_t)size;
    entry[4] = (uint8_t)(selector >> 8);
    entry[5] = (uint8_t)selector;
    size_t n = strlen(name);
    if (n > 56) { n = 56; }
    memcpy(entry + 8, name, n);
    fake_fwcfg_append_item(FWCFG_FILE_DIR, entry, sizeof(entry));
}

static void with_signature(void) {
    fake_fwcfg_reset();
    fake_fwcfg_set_item(FWCFG_SIGNATURE, (const uint8_t *)"QEMU", 4);
}

TEST(fwcfg, a_machine_with_no_such_device_says_so_and_runs_no_tests) {
    /* Real hardware. Every port read returns 0xFF, the signature does not
     * match, and the correct answer to "should this boot run self-tests"
     * is no. This is the default that protects somebody booting from a
     * USB stick, and it is the one case where a wrong answer costs a
     * person a reboot. */
    fake_fwcfg_reset();          /* no items at all: reads return 0xFF */
    fwcfg_init();
    CHECK_EQ(fwcfg_present(), 0);
    CHECK_EQ(boot_selftests_enabled(), 0);
    CHECK(klog_capture_contains("no firmware config device"));
}

TEST(fwcfg, the_signature_must_match_exactly) {
    /* One byte wrong is not a fw_cfg device. A prefix match would find
     * "QEM?" acceptable and then read a directory out of whatever is
     * behind an unclaimed port. */
    static const char *nearly[] = {"QEM", "qemu", "QEMV", "\xff\xff\xff\xff"};
    for (unsigned i = 0; i < sizeof(nearly) / sizeof(nearly[0]); i++) {
        fake_fwcfg_reset();
        fake_fwcfg_set_item(FWCFG_SIGNATURE, (const uint8_t *)nearly[i], 4);
        fwcfg_init();
        CHECK_EQ(fwcfg_present(), 0);
    }
    /* ...and the real one is accepted, because a check that refuses
     * everything passes every assertion above and breaks the machine. */
    with_signature();
    fwcfg_init();
    CHECK_EQ(fwcfg_present(), 1);

    /* "QEMUX" is deliberately NOT in the list above, and finding out why
     * is what this test was worth writing for. The signature item is four
     * bytes; an item whose first four are "QEMU" is a QEMU signature
     * regardless of what follows, so accepting it is correct rather than
     * sloppy. The first draft of this test asserted the opposite and was
     * wrong - recorded here because the next person to read the check
     * will have the same doubt. */
    fake_fwcfg_reset();
    fake_fwcfg_set_item(FWCFG_SIGNATURE, (const uint8_t *)"QEMUX", 5);
    fwcfg_init();
    CHECK_EQ(fwcfg_present(), 1);
}

TEST(fwcfg, the_selftest_file_is_read_and_only_exactly_one_enables_them) {
    /* "1" turns them on. Nothing else does - not "0", not "true", not an
     * empty file, not "11". fwcfg.c's own comment gives the reason: a
     * typo in a harness invocation should fail closed and be noticed,
     * rather than silently enabling a 190-second boot. */
    static const struct { const char *value; int expect; } cases[] = {
        {"1", 1}, {"0", 0}, {"", 0}, {"11", 0}, {"true", 0},
        {"1\n", 0}, {" 1", 0}, {"2", 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        with_signature();
        dir_begin(1);
        dir_add("opt/leanos/selftest", (uint32_t)strlen(cases[i].value), 0x0100);
        fake_fwcfg_set_item(0x0100, (const uint8_t *)cases[i].value,
                            (uint32_t)strlen(cases[i].value));
        fwcfg_init();
            CHECK_EQ(boot_selftests_enabled(), cases[i].expect);
    }
}

TEST(fwcfg, a_present_device_with_no_such_file_runs_no_tests) {
    with_signature();
    dir_begin(2);
    dir_add("etc/something-else", 4, 0x0100);
    dir_add("opt/leanos/other", 4, 0x0101);
    fwcfg_init();
    CHECK_EQ(fwcfg_present(), 1);
    CHECK_EQ(boot_selftests_enabled(), 0);
    CHECK_EQ(fwcfg_read_file("opt/leanos/selftest", (char[8]){0}, 8), -1);
}

TEST(fwcfg, a_name_that_is_a_prefix_of_the_wanted_one_does_not_match) {
    /* The directory's name field is NUL-padded rather than
     * NUL-terminated when it uses all 56 bytes, which is why fwcfg.c
     * compares within the field instead of trusting a terminator. These
     * are the comparisons that mistake gets wrong. */
    static const char *wrong[] = {
        "opt/leanos/selftes",       /* one short */
        "opt/leanos/selftests",     /* one long */
        "opt/leanos/SELFTEST",      /* case */
        "opt/leanos/",              /* a prefix */
    };
    for (unsigned i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
        with_signature();
        dir_begin(1);
        dir_add(wrong[i], 1, 0x0100);
        fake_fwcfg_set_item(0x0100, (const uint8_t *)"1", 1);
        fwcfg_init();
            CHECK_EQ(boot_selftests_enabled(), 0);
    }
}

TEST(fwcfg, a_file_after_the_match_does_not_disturb_it) {
    /* fwcfg.c reads the whole directory even after finding its entry,
     * because the device has no seek and leaving the stream mid-item
     * would corrupt the next selection. This is that behaviour: the
     * wanted file is first, and three entries follow it. */
    with_signature();
    dir_begin(4);
    dir_add("opt/leanos/selftest", 1, 0x0100);
    dir_add("etc/a", 100, 0x0101);
    dir_add("etc/b", 100, 0x0102);
    dir_add("etc/c", 100, 0x0103);
    fake_fwcfg_set_item(0x0100, (const uint8_t *)"1", 1);
    fwcfg_init();
    CHECK_EQ(boot_selftests_enabled(), 1);
}

TEST(fwcfg, an_absurd_directory_count_is_refused_rather_than_walked) {
    /* The count comes from outside the machine. fwcfg.c caps it at 1024
     * for that reason and nothing has ever tested the cap - which is the
     * usual state of a bound that has never been reached. Without it
     * this is an unbounded loop of port reads at boot, on a value a
     * hostile or merely broken hypervisor chose. */
    with_signature();
    dir_begin(0xFFFFFFFFu);
    dir_add("opt/leanos/selftest", 1, 0x0100);
    fake_fwcfg_set_item(0x0100, (const uint8_t *)"1", 1);
    fwcfg_init();
    char buf[8];
    CHECK_NO_PANIC(fwcfg_read_file("opt/leanos/selftest", buf, sizeof(buf)));
    CHECK_EQ(fwcfg_read_file("opt/leanos/selftest", buf, sizeof(buf)), -1);
    CHECK_EQ(boot_selftests_enabled(), 0);
}

TEST(fwcfg, a_blob_larger_than_the_buffer_is_truncated_not_overrun) {
    with_signature();
    uint8_t big[512];
    memset(big, 'x', sizeof(big));
    dir_begin(1);
    dir_add("opt/leanos/big", (uint32_t)sizeof(big), 0x0100);
    fake_fwcfg_set_item(0x0100, big, sizeof(big));
    fwcfg_init();

    /* Guard-banded, because the assertion is about what was NOT written.
     * fwcfg.c documents truncation as deliberate; this is the check that
     * it truncates rather than writing 512 bytes into 16. */
    struct { uint8_t pad0[8]; char buf[16]; uint8_t pad1[8]; } g;
    memset(&g, 0xA5, sizeof(g));
    int n = fwcfg_read_file("opt/leanos/big", g.buf, sizeof(g.buf));
    CHECK_EQ(n, (int)sizeof(g.buf));
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(g.pad0[i], 0xA5);
        CHECK_EQ(g.pad1[i], 0xA5);
    }
}

TEST(fwcfg, reading_a_file_twice_gives_the_same_answer) {
    /* The device is a stream with no seek: reading a file leaves the
     * selector somewhere, and a second read has to re-select. A bug here
     * gives the right answer once and garbage afterwards, which is the
     * kind that survives a single-call test. */
    with_signature();
    dir_begin(1);
    dir_add("opt/leanos/selftest", 1, 0x0100);
    fake_fwcfg_set_item(0x0100, (const uint8_t *)"1", 1);
    fwcfg_init();

    char a[8] = {0}, b[8] = {0};
    CHECK_EQ(fwcfg_read_file("opt/leanos/selftest", a, sizeof(a)), 1);
    CHECK_EQ(fwcfg_read_file("opt/leanos/selftest", b, sizeof(b)), 1);
    CHECK_EQ(a[0], '1');
    CHECK_EQ(b[0], '1');
}

TEST(fwcfg, reading_before_init_is_refused) {
    fake_fwcfg_reset();
    fwcfg_init();                /* no device */
    char buf[8];
    CHECK_EQ(fwcfg_read_file("opt/leanos/selftest", buf, sizeof(buf)), -1);
    CHECK_EQ(fwcfg_read_file(NULL, buf, sizeof(buf)), -1);
    CHECK_EQ(fwcfg_read_file("x", NULL, 8), -1);
}
