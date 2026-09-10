/* tests/test_dns.c - M114
 *
 * The resolver, graded where its interesting failures live: a
 * nameserver that answers nothing.
 *
 * ---- why this tier ----------------------------------------------------
 *
 * user_space/lib/dns.c had no host tests at all before M114. Its wire
 * parser was self-tested inside /bin/nslookup - a good instrument, and
 * it is why the parser has never been the bug. What had never been
 * graded anywhere is the part above the parser: WHICH SERVER gets asked,
 * and what happens when it does not answer. That is where the bug was.
 *
 * The bug, so the tests below read as something rather than as
 * coverage: this machine asked exactly one nameserver, the one DHCP
 * handed over. On a network whose router accepted DNS queries and
 * answered none - which is what was actually in front of this project
 * on 2026-09-10 - every name was unresolvable and the browser said
 * "Could not resolve hostname" while UDP to the public internet was
 * working perfectly in the next window. A resolver with one server
 * cannot tell "the network is down" from "this one server is broken",
 * and it cannot recover from the second.
 *
 * None of that is reachable from a booted machine: you cannot ask a
 * working nameserver to start black-holing, and pointing at an address
 * nothing listens on grades ICMP-unreachable instead of silence. So the
 * server behaviour is settable here, one per address - see
 * tests/fakes/fake_user_net.c.
 */
#include "check.h"

#include "dns.h"
#include "fakes.h"

#include <string.h>

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                        ((uint32_t)(c) << 8) | (uint32_t)(d))

#define DHCP_DNS  IP(10, 0, 2, 3)
#define PUBLIC_A  IP(1, 1, 1, 1)
#define PUBLIC_B  IP(8, 8, 8, 8)
#define ANSWER    IP(93, 184, 216, 34)

static void resolv(const char *text) {
    fake_user_net_set_resolv_conf(text, (long)strlen(text));
}

/* The resolver's cache is one static array shared by every test in this
 * process, so a name resolved by one case is answered from memory in the
 * next - which silently turned four of these into tests of the cache.
 * Found the first time they ran: "queries seen: expected 1, got 0", a
 * resolver that had not sent a packet at all and still returned the
 * right address. */
static void fresh(void) {
    fake_user_net_reset();
    dns_cache_clear();
}

/* ---- the file ---------------------------------------------------------- */

TEST(dns, resolv_conf_reads_a_nameserver_line) {
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "nameserver 1.1.1.1\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], PUBLIC_A);
}

TEST(dns, resolv_conf_reads_several_in_order) {
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 2);
    CHECK_EQ(out[0], PUBLIC_A);
    CHECK_EQ(out[1], PUBLIC_B);
}

TEST(dns, resolv_conf_ignores_comments_and_blank_lines) {
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "# nameserver 9.9.9.9\n"
                    "\n"
                    "   ; nameserver 7.7.7.7\n"
                    "nameserver 1.1.1.1   # the real one\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], PUBLIC_A);
}

TEST(dns, resolv_conf_ignores_directives_it_does_not_implement) {
    /* `search` and `options` are a feature each and this file implements
     * neither. Skipping them is honest; reading them as nameservers
     * would not be. */
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "domain example.com\n"
                    "search example.com lan\n"
                    "options ndots:2\n"
                    "nameservers 5.5.5.5\n"   /* not the keyword */
                    "nameserver 1.1.1.1\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], PUBLIC_A);
}

TEST(dns, resolv_conf_refuses_addresses_that_are_not_addresses) {
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "nameserver 1.1.1\n"          /* three octets */
                    "nameserver 1.1.1.1.1\n"      /* five - must not read as 1.1.1.1 */
                    "nameserver 256.1.1.1\n"      /* out of range */
                    "nameserver 1.1.1.0001\n"     /* padded past three digits */
                    "nameserver ::1\n"            /* v6, which this stack has none of */
                    "nameserver\n"                /* no argument at all */
                    "nameserver 0.0.0.0\n"        /* not a server */
                    "nameserver 8.8.8.8\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], PUBLIC_B);
}

TEST(dns, resolv_conf_deduplicates) {
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "nameserver 1.1.1.1\nnameserver 1.1.1.1\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
}

TEST(dns, resolv_conf_stops_at_the_caller_s_limit) {
    uint32_t out[2];
    const char *t = "nameserver 1.1.1.1\nnameserver 8.8.8.8\nnameserver 9.9.9.9\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, 2), 2);
}

TEST(dns, resolv_conf_without_a_trailing_newline_still_parses) {
    /* An editor that does not add one is not a syntax error, and this is
     * the last line in the file - so it is the line most likely to be
     * the only one. */
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "nameserver 1.1.1.1";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], PUBLIC_A);
}

/* ---- the list --------------------------------------------------------- */

TEST(dns, the_server_list_is_the_file_then_dhcp) {
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");

    uint32_t out[DNS_MAX_SERVERS];
    CHECK_EQ(dns_servers(out, DNS_MAX_SERVERS), 2);
    /* The file first: it is what a person edited on purpose, and DHCP is
     * what a machine on the other end of a cable decided. */
    CHECK_EQ(out[0], PUBLIC_A);
    CHECK_EQ(out[1], DHCP_DNS);
}

TEST(dns, with_no_resolv_conf_the_list_is_what_dhcp_said) {
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    fake_user_net_no_resolv_conf();

    uint32_t out[DNS_MAX_SERVERS];
    CHECK_EQ(dns_servers(out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], DHCP_DNS);
}

TEST(dns, dhcp_is_not_listed_twice_when_the_file_already_names_it) {
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 10.0.2.3\n");

    uint32_t out[DNS_MAX_SERVERS];
    CHECK_EQ(dns_servers(out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], DHCP_DNS);
}

TEST(dns, a_resolv_conf_larger_than_the_read_buffer_does_not_overrun_it) {
    /* The bug M114 shipped and then found on the machine. sys_readfile
     * returns the FILE's size and copies at most maxlen, so a file
     * bigger than the buffer returns a length longer than the bytes it
     * wrote. Handing that number to the parser reads off the end of the
     * array; ASan is what turns this test into a failure rather than a
     * plausible answer, which is why it is in this tier and could not
     * have been anywhere else.
     *
     * The nameserver is at the very top so that a correct read still
     * finds it - this test must fail on the overread, not on the
     * truncation. */
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);

    static char big[8192];
    memset(big, '\0', sizeof(big));
    const char *head = "nameserver 1.1.1.1\n";
    memcpy(big, head, strlen(head));
    for (size_t i = strlen(head); i < sizeof(big) - 1; i++) {
        big[i] = (i % 64 == 63) ? '\n' : '#';
    }
    big[sizeof(big) - 1] = '\n';
    fake_user_net_set_resolv_conf(big, (long)sizeof(big));

    uint32_t out[DNS_MAX_SERVERS];
    int n = dns_servers(out, DNS_MAX_SERVERS);
    CHECK(n >= 1);
    CHECK_EQ(out[0], PUBLIC_A);
}

/* ---- resolving -------------------------------------------------------- */

TEST(dns, a_name_resolves_when_the_only_server_answers) {
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    fake_user_net_no_resolv_conf();
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_ANSWER, ANSWER);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("example.com", &ip), 0);
    CHECK_EQ(ip, ANSWER);
}

TEST(dns, a_silent_first_server_does_not_stop_the_second_from_answering) {
    /* THE test. This is the machine M114 was written on: the DHCP
     * nameserver accepts the query and says nothing, and before this
     * milestone that was the end of it - one server, three seconds,
     * "timed out", and a browser reporting that it could not resolve a
     * hostname on a network that was working. */
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_SILENT, 0);
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_ANSWER, ANSWER);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("example.com", &ip), 0);
    CHECK_EQ(ip, ANSWER);
    /* And it really did ask the broken one too - otherwise this test
     * would pass just as well against a resolver that had quietly
     * stopped using DHCP's server at all, which would be a different
     * bug wearing this fix's clothes. */
    CHECK(fake_user_net_queries_seen(DHCP_DNS) >= 1);
}

TEST(dns, the_silent_server_costs_no_extra_round_trips) {
    /* Asked in parallel rather than in turn: both servers see a query in
     * the first round, so the live one answers on the first poll instead
     * of after the dead one's whole timeout. A sequential resolver would
     * show 0 queries at the second address until the first had timed
     * out. */
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_SILENT, 0);
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_ANSWER, ANSWER);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("example.com", &ip), 0);
    CHECK_EQ(fake_user_net_queries_seen(DHCP_DNS), 1);
    CHECK_EQ(fake_user_net_queries_seen(PUBLIC_A), 1);
}

TEST(dns, every_server_silent_is_a_timeout_and_not_a_hang) {
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_SILENT, 0);
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_SILENT, 0);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("nothing.example", &ip), -2);
}

TEST(dns, a_definite_no_stops_the_lookup) {
    /* NXDOMAIN is an answer. A resolver that kept waiting after one
     * would turn "this name does not exist" into a timeout, which is the
     * failure mode that sends somebody debugging their network instead
     * of their typing. */
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_NXDOMAIN, 0);
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_NXDOMAIN, 0);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("nope.example", &ip), -3);
}

TEST(dns, an_address_from_one_server_beats_a_no_from_another) {
    /* A consequence of asking every server at once, written down as a
     * test because it is a real semantic choice rather than an accident:
     * if one nameserver has a name and another does not, this machine
     * uses the address. That is the right answer for the case this
     * milestone was built for - a server that is wrong about the world -
     * and the wrong one for split-horizon DNS, where the local server is
     * authoritative for a name the public one has never heard of.
     *
     * Which way round it goes here is decided by ORDER: resolv.conf's
     * servers are asked before DHCP's, so the file wins the race. That
     * is why the file is first, and it is why this test's premise was
     * wrong the first time it was written - it expected the DHCP
     * server's "no" to win, and the file's address arrived instead. */
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_ANSWER, ANSWER);
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_NXDOMAIN, 0);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("split.example", &ip), 0);
    CHECK_EQ(ip, ANSWER);
}

TEST(dns, a_reply_that_is_not_dns_does_not_end_the_lookup) {
    /* A garbage datagram from one server must not be taken as the
     * answer, and must not stop a real one arriving from another. */
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_GARBAGE, 0);
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_ANSWER, ANSWER);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("example.com", &ip), 0);
    CHECK_EQ(ip, ANSWER);
}

TEST(dns, with_no_server_anywhere_the_answer_is_immediate_and_distinct) {
    /* -1, not -2: "nobody is configured" and "nobody answered" are
     * different things to tell a person, and dns.h documents them as
     * five distinct codes for exactly that reason. */
    fresh();
    fake_user_net_set_dhcp_dns(0);
    fake_user_net_no_resolv_conf();

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("example.com", &ip), -1);
}

TEST(dns, a_socket_that_cannot_be_opened_is_reported_as_such) {
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    fake_user_net_no_resolv_conf();
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_ANSWER, ANSWER);
    fake_user_net_set_socket_fails(1);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("example.com", &ip), -1);
}
