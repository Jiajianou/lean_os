/* user_space/bin/fetch.c
 *
 * M73: the first program that can put something on this machine that was
 * not compiled into its disk image.
 *
 * That is the whole significance of it. M63 ran somebody else's program
 * and M65 built a capability model arguing that a *downloaded* program is
 * a real category - both reasoning about something that could not yet
 * happen here. After this it can, which is also why this program holds
 * CAP_NETWORK and an ordinary one does not.
 *
 *   fetch URL              print the body
 *   fetch URL FILE         write the body to FILE
 */
#include <stdio.h>
#include <string.h>

#include "http.h"
#include "syscall_wrappers.h"

/* Bounded on purpose. There is no streaming here - the body is held whole
 * so that writing it to a file is one call - so the cap is also the
 * promise that a hostile or enormous response cannot exhaust this
 * program's heap. */
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
        /* Printed even when the body is empty: "404 with nothing in it"
         * and "200 with nothing in it" are different answers. */
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
