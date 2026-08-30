/* user_space/lib/http.h
 *
 * M73: an HTTP/1.1 GET, and the first way anything has ever arrived on
 * this machine without being compiled into its disk image.
 *
 * That is the point of it rather than a side effect. M63 ran somebody
 * else's program; M65 built a capability model on the argument that a
 * downloaded program is a real category. Both were reasoning about a
 * thing that could not yet happen. This is the call that makes it happen,
 * which is also why `fetch` holds CAP_NETWORK and an ordinary program
 * does not.
 *
 * DELIBERATELY NO TLS. A from-scratch TLS 1.3 is a project rather than a
 * bullet, and an `https://` that quietly was not encrypted would be a lie
 * of exactly the kind this file keeps refusing to tell - so `https` is
 * refused by name rather than downgraded.
 */
#pragma once

#include <stdint.h>

#define HTTP_MAX_REDIRECTS 3

/* GET http://host[:port]/path into `body` (at most `cap` bytes).
 *
 * Returns the number of body bytes on success, or:
 *   -1  malformed URL, or an https:// one
 *   -2  the name did not resolve
 *   -3  could not connect
 *   -4  the response was malformed, or the headers did not fit
 *   -5  too many redirects
 *   -6  the body exceeded `cap`
 *
 * `*status_out` (may be NULL) receives the HTTP status code even when the
 * body is empty, because "404 with nothing in it" and "200 with nothing
 * in it" are different answers and a caller that cannot tell them apart
 * cannot report either. */
long http_get(const char *url, char *body, long cap, int *status_out);
