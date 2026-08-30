/* user_space/lib/dns.h
 *
 * M73: turning a name into an address.
 *
 * M64 shipped `SYS_netconf` returning a DNS server address and wrote,
 * carefully, that it prints "the DNS server it was handed instead of
 * pretending to use it". This is the thing that stops pretending.
 *
 * In USER SPACE, and deliberately, following M64's own precedent for
 * SNTP: "a kernel parsing replies off the network is a far larger trusted
 * surface than a sixty-line program needs". A DNS response is a strictly
 * nastier piece of untrusted input than an SNTP reply - it contains
 * length-prefixed labels, a pointer format that can point backwards into
 * the message, and a record count the sender chooses - so it is exactly
 * the parser that should not be in the kernel.
 */
#pragma once

#include <stdint.h>

#define DNS_MAX_NAME 255

/* Resolve `name` to an IPv4 address in host order (10.0.2.2 is
 * 0x0A000202, the convention kernel/net/net.h uses and for the reason it
 * gives: this OS never byte-swaps an address into a register).
 *
 * Returns 0 and fills `*out` on success, or:
 *   -1  bad argument, no socket, or no DNS server configured
 *   -2  timed out - the server never answered
 *   -3  the server answered with an error (NXDOMAIN and friends)
 *   -4  the reply was malformed, or answered a question we did not ask
 *
 * Five distinct failures rather than one, because "the name does not
 * exist" and "the network is down" want different things from a caller
 * and a resolver that conflates them makes both undiagnosable.
 *
 * Blocking, with a bounded deadline. A cache in front of it means a
 * repeated lookup costs nothing; entries expire on the TTL the server
 * gave rather than on a number invented here. */
int dns_resolve(const char *name, uint32_t *out);

/* Exposed for the self-test: parse a raw response as if it had arrived
 * off the wire. The parser is the part with the interesting failure
 * modes, and it is the part that can be checked without a network. */
int dns_parse_response(const uint8_t *msg, int len, uint16_t expect_id,
                        const char *expect_name, uint32_t *out);

/* Build a query for `name` into `buf`; returns its length or -1. */
int dns_build_query(const char *name, uint16_t id, uint8_t *buf, int cap);
