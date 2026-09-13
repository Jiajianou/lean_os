#ifndef LEANOS_TESTS_CHECK_H
#define LEANOS_TESTS_CHECK_H

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

#endif
