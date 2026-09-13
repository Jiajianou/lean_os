#include "check.h"

void fake_spinlock_release_all(void);

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static test_case_t *head;
static test_case_t *tail;

static int current_failures;
static int total_failures;
static int total_tests;
static int total_passed;
static jmp_buf abandon_jmp;
static int abandon_armed;

jmp_buf test_panic_jmp;
int test_panic_armed;
char test_panic_msg[256];

void test_register(test_case_t *tc) {
    tc->next = NULL;
    if (tail) {
        tail->next = tc;
    } else {
        head = tc;
    }
    tail = tc;
}

void test_fail(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    current_failures++;
    total_failures++;
    fprintf(stderr, "    FAIL %s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void test_abandon(void) {
    if (abandon_armed) {
        longjmp(abandon_jmp, 1);
    }
    fprintf(stderr, "    (test_abandon outside a test - harness bug)\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *filter = NULL;
    int run_slow = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--slow") == 0) {
            run_slow = 1;
        } else {
            filter = argv[i];
        }
    }
    const char *last_suite = NULL;
    int skipped_slow = 0;

    for (test_case_t *tc = head; tc; tc = tc->next) {
        if (filter && !strstr(tc->suite, filter) && !strstr(tc->name, filter)) {
            continue;
        }
        if (!run_slow && !filter && strncmp(tc->name, "slow_", 5) == 0) {
            skipped_slow++;
            continue;
        }
        if (!last_suite || strcmp(last_suite, tc->suite) != 0) {
            printf("%s\n", tc->suite);
            last_suite = tc->suite;
        }

        current_failures = 0;
        total_tests++;

        abandon_armed = 1;
        if (setjmp(abandon_jmp) == 0) {
            tc->fn();
        }
        abandon_armed = 0;
        test_panic_armed = 0;
        fake_spinlock_release_all();

        if (current_failures == 0) {
            total_passed++;
            printf("  ok   %s\n", tc->name);
        } else {
            printf("  FAIL %s (%d assertion%s)\n", tc->name, current_failures,
                   current_failures == 1 ? "" : "s");
        }
    }

    printf("\n%d/%d tests passed", total_passed, total_tests);
    if (skipped_slow) {
        printf(", %d slow test%s skipped (--slow runs them)", skipped_slow,
               skipped_slow == 1 ? "" : "s");
    }
    if (total_failures) {
        printf(", %d failed assertion%s", total_failures,
               total_failures == 1 ? "" : "s");
    }
    printf("\n");

    if (total_tests == 0) {
        fprintf(stderr, "no tests ran%s%s\n", filter ? " matching " : "",
                filter ? filter : "");
        return 2;
    }
    return total_failures ? 1 : 0;
}
