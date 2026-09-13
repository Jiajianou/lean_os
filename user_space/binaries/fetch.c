#include <stdio.h>
#include <string.h>

#include "http.h"
#include "syscall_wrappers.h"

#define FETCH_MAX 65536

static char body[FETCH_MAX];

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1][0]) {
        printf("usage: fetch URL [FILE]\n");
        return 2;
    }

    int status = 0;
    long n = http_get(argv[1], body, sizeof(body), &status);
    if (n < 0) {
        static const char *why[] = {
            "", "not a URL this client can fetch (https is refused, not downgraded)",
            "the name did not resolve", "could not connect",
            "the reply was malformed", "too many redirects", "the body was too large",
        };
        long idx = -n;
        printf("fetch: %s\n", (idx >= 1 && idx <= 6) ? why[idx] : "failed");
        return 1;
    }

    if (status < 200 || status >= 300) {
        printf("fetch: server returned %d\n", status);
    }

    if (argc >= 3 && argv[2][0]) {
        if (sys_writefile(argv[2], body, (size_t)n) != 0) {
            printf("fetch: could not write %s\n", argv[2]);
            return 1;
        }
        printf("fetch: %ld byte(s) -> %s\n", n, argv[2]);
    } else {
        sys_write(1, body, (size_t)n);
    }
    return (status >= 200 && status < 300) ? 0 : 1;
}
