#include "../check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *msg);

void panic(const char *msg) {
    snprintf(test_panic_message, sizeof(test_panic_message), "%s", msg ? msg : "(null)");
    if (test_panic_armed) {
        longjmp(test_panic_jmp, 1);
    }
    fprintf(stderr, "\n*** unexpected KERNEL PANIC in a host test: %s\n",
            test_panic_message);
    exit(3);
}
