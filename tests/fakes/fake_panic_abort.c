#include <stdio.h>
#include <stdlib.h>

void panic(const char *message);

void panic(const char *message) {
    fprintf(stderr, "*** KERNEL PANIC reached by a fuzz input: %s\n",
            message ? message : "(null)");
    abort();
}
