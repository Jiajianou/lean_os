/* tests/check.h - Q2
 *
 * The whole test framework. There is not a lot of it on purpose.
 *
 * ---- Why this exists at all ------------------------------------------
 *
 * For ninety-three milestones every check in this project cost a QEMU
 * boot, because every check *was* a boot: a self-test compiled into
 * kernel.c, run on the real machine, graded by grepping a serial log.
 * That is the right instrument for "does the filesystem work when it is
 * mounted on a real disk under a real scheduler", and it is why this
 * project has the regression record it has.
 *
 * What it cannot do is reach an error path. Getting to "the disk is
 * full" from inside a booted OS means filling the disk; getting to
 * "kmalloc returned NULL" means exhausting the heap; getting to "the
 * superblock is corrupt" means corrupting one. So those branches - and
 * there are several hundred of them in this kernel - have never been
 * executed by anything. This is the tier that reaches them, and it is
 * additive: not one boot self-test was deleted to make room for it.
 *
 * The second reason is the feedback loop. A check that takes 200
 * milliseconds is a check that runs while you type.
 *
 * ---- Why it is written here rather than fetched ----------------------
 *
 * Same reason as everything else in this tree. A test framework is a
 * `CHECK` macro, a counter and a list; Unity and Check and cmocka are
 * that plus thirty thousand lines of things this project will not use.
 * The one dependency taken anywhere near this is libFuzzer in Q4, and
 * that is a compiler flag rather than a library.
 *
 * ---- The part that is not obvious: catching a panic ------------------
 *
 * A kernel's answer to "you did something impossible" is panic(), which
 * never returns. Two hundred and nineteen calls to it exist and no test
 * has ever executed one, because on the real machine executing one ends
 * the test run. tests/fakes/fake_panic.c makes panic() a longjmp back to
 * here, so a test can assert *that* the kernel panicked and on what
 * message - see CHECK_PANIC. That turns a whole category of "this must
 * never happen" comment into something gradeable.
 */
#ifndef LEANOS_TESTS_CHECK_H
#define LEANOS_TESTS_CHECK_H

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Registration ----------------------------------------------------
 *
 * A constructor per test, so a test file is added to the build and needs
 * no second edit anywhere to be run. The failure mode this avoids is the
 * one worth avoiding: a test that is written, compiled, and silently
 * never called because a list somewhere was not updated. */
typedef void (*test_fn_t)(void);

typedef struct test_case {
    const char *suite;
    const char *name;
    test_fn_t fn;
    struct test_case *next;
} test_case_t;

void test_register(test_case_t *tc);

#define TEST(suite_, name_)                                                   \
    static void test_##suite_##_##name_(void);                                \
    static test_case_t tc_##suite_##_##name_ = {                              \
        #suite_, #name_, test_##suite_##_##name_, NULL};                      \
    __attribute__((constructor)) static void reg_##suite_##_##name_(void) {    \
        test_register(&tc_##suite_##_##name_);                                \
    }                                                                         \
    static void test_##suite_##_##name_(void)

/* ---- Assertions ------------------------------------------------------
 *
 * CHECK records a failure and keeps going; REQUIRE records one and
 * abandons the test. The distinction matters more than it looks: a test
 * that stops at its first failed assertion tells you one thing per run,
 * and a test that keeps going tells you the shape of the breakage. But a
 * test that keeps going *past a null pointer* segfaults and tells you
 * nothing, which is what REQUIRE is for. */
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void test_abandon(void);

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);                 \
        }                                                                      \
    } while (0)

#define REQUIRE(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            test_fail(__FILE__, __LINE__, "REQUIRE(%s)", #cond);               \
            test_abandon();                                                    \
        }                                                                      \
    } while (0)

/* Values printed on failure, because "CHECK(n == expected) failed" is a
 * bisect and "expected 4096, got 4088" is a diagnosis. */
#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        long long a_ = (long long)(actual);                                    \
        long long e_ = (long long)(expected);                                  \
        if (a_ != e_) {                                                        \
            test_fail(__FILE__, __LINE__, "%s == %s: expected %lld, got %lld",  \
                      #actual, #expected, e_, a_);                             \
        }                                                                      \
    } while (0)

#define CHECK_NE(actual, unexpected)                                           \
    do {                                                                       \
        long long a_ = (long long)(actual);                                    \
        long long u_ = (long long)(unexpected);                                \
        if (a_ == u_) {                                                        \
            test_fail(__FILE__, __LINE__, "%s != %s: both are %lld",           \
                      #actual, #unexpected, u_);                               \
        }                                                                      \
    } while (0)

#define CHECK_STREQ(actual, expected)                                          \
    do {                                                                       \
        const char *a_ = (actual);                                             \
        const char *e_ = (expected);                                           \
        if (!a_ || !e_ || strcmp(a_, e_) != 0) {                               \
            test_fail(__FILE__, __LINE__, "%s == \"%s\": got \"%s\"",          \
                      #actual, e_ ? e_ : "(null)", a_ ? a_ : "(null)");        \
        }                                                                      \
    } while (0)

#define CHECK_MEMEQ(actual, expected, n)                                       \
    do {                                                                       \
        size_t n_ = (size_t)(n);                                               \
        const unsigned char *a_ = (const unsigned char *)(actual);             \
        const unsigned char *e_ = (const unsigned char *)(expected);           \
        size_t i_ = 0;                                                         \
        for (; i_ < n_; i_++) {                                                \
            if (a_[i_] != e_[i_]) {                                            \
                test_fail(__FILE__, __LINE__,                                  \
                          "%s == %s: first difference at byte %zu "            \
                          "(expected 0x%02x, got 0x%02x)",                     \
                          #actual, #expected, i_, e_[i_], a_[i_]);             \
                break;                                                         \
            }                                                                  \
        }                                                                      \
    } while (0)

/* ---- Panic assertions ------------------------------------------------
 *
 * CHECK_PANIC(stmt, "substring") runs stmt and passes only if it reached
 * panic() with a message containing that substring. CHECK_NO_PANIC is the
 * other half and is the one that catches the more common bug: a
 * defensive check that fires when it should not.
 *
 * The substring is required rather than optional. "It panicked" is not an
 * assertion worth making - a test that passes on *any* panic passes when
 * the code panics for a completely unrelated reason, which is exactly how
 * a test stops meaning what its name says. */
extern jmp_buf test_panic_jmp;
extern int test_panic_armed;
extern char test_panic_msg[256];

#define CHECK_PANIC(stmt, substr)                                              \
    do {                                                                       \
        test_panic_msg[0] = '\0';                                              \
        test_panic_armed = 1;                                                  \
        if (setjmp(test_panic_jmp) == 0) {                                     \
            stmt;                                                              \
            test_panic_armed = 0;                                              \
            test_fail(__FILE__, __LINE__,                                      \
                      "expected a panic matching \"%s\", but %s returned",     \
                      (substr), #stmt);                                        \
        } else {                                                               \
            test_panic_armed = 0;                                              \
            if (!strstr(test_panic_msg, (substr))) {                           \
                test_fail(__FILE__, __LINE__,                                  \
                          "panicked, but on \"%s\" rather than \"%s\"",        \
                          test_panic_msg, (substr));                           \
            }                                                                  \
        }                                                                      \
    } while (0)

#define CHECK_NO_PANIC(stmt)                                                   \
    do {                                                                       \
        test_panic_msg[0] = '\0';                                              \
        test_panic_armed = 1;                                                  \
        if (setjmp(test_panic_jmp) == 0) {                                     \
            stmt;                                                              \
            test_panic_armed = 0;                                              \
        } else {                                                               \
            test_panic_armed = 0;                                              \
            test_fail(__FILE__, __LINE__, "unexpected panic: \"%s\"",          \
                      test_panic_msg);                                         \
        }                                                                      \
    } while (0)

#endif /* LEANOS_TESTS_CHECK_H */
