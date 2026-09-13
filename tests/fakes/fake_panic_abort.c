#include <stdio.h>
#include <stdlib.h>

void panic(const char *msg);

void panic(const char *msg) {
    fprintf(stderr, "*** KERNEL PANIC reached by a fuzz input: %s\n",
            msg ? msg : "(null)");
    abort();
}
