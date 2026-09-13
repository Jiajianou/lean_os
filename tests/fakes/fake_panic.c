#include "../check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *message);

void panic(const char *message) {
    snprintf(test_panic_message, sizeof(test_panic_message), "%s", message ? message : "(null)");
    if (test_panic_armed) {
        longjmp(test_panic_jmp, 1);
    }
    fprintf(stderr, "\n*** unexpected KERNEL PANIC in a host test: %s\n",
            test_panic_message);
    exit(3);
}
