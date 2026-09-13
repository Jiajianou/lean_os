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

static void fresh(void) {
    fake_user_net_reset();
    dns_cache_clear();
}

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
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "domain example.com\n"
                    "search example.com lan\n"
                    "options ndots:2\n"
                    "nameservers 5.5.5.5\n"
                    "nameserver 1.1.1.1\n";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], PUBLIC_A);
}

TEST(dns, resolv_conf_refuses_addresses_that_are_not_addresses) {
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "nameserver 1.1.1\n"
                    "nameserver 1.1.1.1.1\n"
                    "nameserver 256.1.1.1\n"
                    "nameserver 1.1.1.0001\n"
                    "nameserver ::1\n"
                    "nameserver\n"
                    "nameserver 0.0.0.0\n"
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
    uint32_t out[DNS_MAX_SERVERS];
    const char *t = "nameserver 1.1.1.1";
    CHECK_EQ(dns_parse_resolv_conf(t, (int)strlen(t), out, DNS_MAX_SERVERS), 1);
    CHECK_EQ(out[0], PUBLIC_A);
}

TEST(dns, the_server_list_is_the_file_then_dhcp) {
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");

    uint32_t out[DNS_MAX_SERVERS];
    CHECK_EQ(dns_servers(out, DNS_MAX_SERVERS), 2);
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
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_SILENT, 0);
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_ANSWER, ANSWER);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("example.com", &ip), 0);
    CHECK_EQ(ip, ANSWER);
    CHECK(fake_user_net_queries_seen(DHCP_DNS) >= 1);
}

TEST(dns, the_silent_server_costs_no_extra_round_trips) {
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
    fresh();
    fake_user_net_set_dhcp_dns(DHCP_DNS);
    resolv("nameserver 1.1.1.1\n");
    fake_user_net_add_server(DHCP_DNS, FAKE_DNS_NXDOMAIN, 0);
    fake_user_net_add_server(PUBLIC_A, FAKE_DNS_NXDOMAIN, 0);

    uint32_t ip = 0;
    CHECK_EQ(dns_resolve("nope.example", &ip), -3);
}

TEST(dns, an_address_from_one_server_beats_a_no_from_another) {
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
