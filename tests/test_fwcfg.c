#include "check.h"
#include "fakes/fakes.h"
#include "dev/fwcfg.h"

#include <stdint.h>
#include <string.h>

#define FWCFG_SIGNATURE 0x0000
#define FWCFG_FILE_DIR  0x0019

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
    fake_fwcfg_reset();
    fwcfg_init();
    CHECK_EQ(fwcfg_present(), 0);
    CHECK_EQ(boot_selftests_enabled(), 0);
    CHECK(klog_capture_contains("no firmware config device"));
}

TEST(fwcfg, the_signature_must_match_exactly) {
    static const char *nearly[] = {"QEM", "qemu", "QEMV", "\xff\xff\xff\xff"};
    for (unsigned i = 0; i < sizeof(nearly) / sizeof(nearly[0]); i++) {
        fake_fwcfg_reset();
        fake_fwcfg_set_item(FWCFG_SIGNATURE, (const uint8_t *)nearly[i], 4);
        fwcfg_init();
        CHECK_EQ(fwcfg_present(), 0);
    }
    with_signature();
    fwcfg_init();
    CHECK_EQ(fwcfg_present(), 1);

    fake_fwcfg_reset();
    fake_fwcfg_set_item(FWCFG_SIGNATURE, (const uint8_t *)"QEMUX", 5);
    fwcfg_init();
    CHECK_EQ(fwcfg_present(), 1);
}

TEST(fwcfg, the_selftest_file_is_read_and_only_exactly_one_enables_them) {
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

TEST(fwcfg, the_four_switches_are_read_independently_of_each_other) {
    static const struct {
        const char *name;
        int (*ask)(void);
    } SWITCHES[] = {
        {"opt/leanos/selftest",  boot_selftests_enabled},
        {"opt/leanos/ioapic",    boot_ioapic_enabled},
        {"opt/leanos/bootstrap", boot_bootstrap_enabled},
        {"opt/leanos/pytest",    boot_pytest_enabled},
        {"opt/leanos/pybuild",   boot_pybuild_enabled},
    };
    const unsigned N = sizeof(SWITCHES) / sizeof(SWITCHES[0]);

    for (unsigned i = 0; i < N; i++) {
        with_signature();
        dir_begin((int)N);
        for (unsigned k = 0; k < N; k++) {
            dir_add(SWITCHES[k].name, 1, (uint16_t)(0x0100 + k));
        }
        for (unsigned k = 0; k < N; k++) {
            fake_fwcfg_set_item((uint16_t)(0x0100 + k),
                                (const uint8_t *)"0", 1);
        }
        fake_fwcfg_set_item((uint16_t)(0x0100 + i), (const uint8_t *)"1", 1);
        fwcfg_init();
        for (int round = 0; round < 2; round++) {
            for (unsigned k = 0; k < N; k++) {
                CHECK_EQ(SWITCHES[k].ask(), k == i ? 1 : 0);
            }
        }
    }
}

TEST(fwcfg, a_machine_with_no_bootstrap_file_runs_no_build) {
    with_signature();
    dir_begin(1);
    dir_add("opt/leanos/selftest", 1, 0x0100);
    fake_fwcfg_set_item(0x0100, (const uint8_t *)"1", 1);
    fwcfg_init();
    CHECK_EQ(boot_selftests_enabled(), 1);
    CHECK_EQ(boot_bootstrap_enabled(), 0);
    CHECK_EQ(boot_pytest_enabled(), 0);
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
    static const char *wrong[] = {
        "opt/leanos/selftes",
        "opt/leanos/selftests",
        "opt/leanos/SELFTEST",
        "opt/leanos/",
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
    fwcfg_init();
    char buf[8];
    CHECK_EQ(fwcfg_read_file("opt/leanos/selftest", buf, sizeof(buf)), -1);
    CHECK_EQ(fwcfg_read_file(NULL, buf, sizeof(buf)), -1);
    CHECK_EQ(fwcfg_read_file("x", NULL, 8), -1);
}
