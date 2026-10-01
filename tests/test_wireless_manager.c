#include "check.h"

#include "network/wireless_manager.h"
#include "network/wireless_simulator.h"
#include "network/wpa_crypto.h"

/* The wireless manager over the simulated radio, with no task: every step is
   a call, and time is an argument. The access points on the other side hold
   their own passphrases and run the authenticator's half of the handshake,
   so "joined" here means the station proved it knew the password and
   installed the keys the access point derived - the simulator refuses
   anything else. The authenticator's own correctness is graded separately,
   against Python's, in test_wpa_handshake.c. */

void fake_wireless_glue_reset(int keep_storage);
const char *fake_wireless_storage(int *length);
void fake_wireless_set_storage(const char *text);
void fake_wireless_set_addressing(int answer, int link_free);
uint32_t fake_wireless_addressing_started(void);
const uint8_t *fake_wireless_delivered(uint32_t index, uint32_t *length);
uint32_t fake_wireless_delivered_count(void);

static uint64_t now;

static void start(int keep_storage) {
    fake_wireless_glue_reset(keep_storage);
    wireless_manager_reset(wireless_simulator_backend());
    now = 1000;
}

static void steps(int count) {
    for (int i = 0; i < count; i++) {
        now += 20;
        wireless_step(now);
    }
}

static os_wireless_status_t status(void) {
    os_wireless_status_t s;
    wireless_status(&s);
    return s;
}

static void request(const char *ssid, const char *password) {
    os_wireless_connect_t r;
    memset(&r, 0, sizeof(r));
    r.ssid_length = (uint8_t)strlen(ssid);
    memcpy(r.ssid, ssid, r.ssid_length);
    snprintf(r.password, sizeof(r.password), "%s", password);
    r.remember = 1;
    REQUIRE(wireless_request_connect(&r) == 0);
}

static int find_row(const os_wireless_network_t *rows, long count, const char *ssid) {
    for (long i = 0; i < count; i++) {
        if (strcmp(rows[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

TEST(wireless_manager, a_scan_lists_one_row_per_name_strongest_first) {
    start(0);
    steps(2);
    os_wireless_network_t rows[WIRELESS_NETWORKS_MAX];
    long count = wireless_networks(rows, WIRELESS_NETWORKS_MAX);
    REQUIRE(count == 4);
    CHECK_EQ(strcmp(rows[0].ssid, "Workshop"), 0);
    CHECK_EQ(rows[0].signal_dbm, -45);
    CHECK_EQ(rows[0].channel, 6);
    CHECK(rows[0].supported);
    CHECK_EQ(rows[0].security, WIRELESS_SECURITY_WPA2_PSK);
    CHECK_EQ(strcmp(rows[1].ssid, "Corner Cafe"), 0);
    CHECK_EQ(rows[1].security, WIRELESS_SECURITY_OPEN);
    CHECK(rows[1].supported);
    CHECK_EQ(strcmp(rows[2].ssid, "Laboratory"), 0);
    CHECK_EQ(rows[2].security, WIRELESS_SECURITY_WPA3_SAE);
    CHECK(!rows[2].supported);
    CHECK_EQ(strcmp(rows[3].ssid, "Neighbours"), 0);
    for (long i = 0; i < count; i++) {
        CHECK(!rows[i].known);
        CHECK(!rows[i].connected);
    }
    CHECK_EQ(status().state, WIRELESS_STATE_IDLE);
}

TEST(wireless_manager, the_right_password_joins_and_is_remembered) {
    start(0);
    steps(2);
    request("Workshop", "lean os wireless");
    CHECK_EQ(status().state, WIRELESS_STATE_JOINING);
    steps(3);
    os_wireless_status_t s = status();
    CHECK_EQ(s.state, WIRELESS_STATE_CONNECTED);
    CHECK_EQ(s.error, WIRELESS_ERROR_NONE);
    CHECK_EQ(s.ip, 0xC0A84D17u);
    CHECK_EQ(strcmp(s.ssid, "Workshop"), 0);
    CHECK_EQ(s.channel, 6);
    CHECK(wireless_simulator_authorized());
    CHECK_EQ(wireless_simulator_keys_wrong(), 0u);
    CHECK_EQ(fake_wireless_addressing_started(), 1u);

    int length = 0;
    const char *stored = fake_wireless_storage(&length);
    REQUIRE(length == 16 + 1 + 64 + 1);
    CHECK_EQ(memcmp(stored, "576f726b73686f70 ", 17), 0);
    CHECK(strstr(stored, "lean") == 0);

    os_wireless_network_t rows[WIRELESS_NETWORKS_MAX];
    long count = wireless_networks(rows, WIRELESS_NETWORKS_MAX);
    int row = find_row(rows, count, "Workshop");
    REQUIRE(row >= 0);
    CHECK(rows[row].known);
    CHECK(rows[row].connected);
}

TEST(wireless_manager, a_wrong_password_is_named_as_one_and_not_remembered) {
    start(0);
    steps(2);
    request("Workshop", "lean os wirelesz");
    steps(5);
    os_wireless_status_t s = status();
    CHECK_EQ(s.state, WIRELESS_STATE_FAILED);
    CHECK_EQ(s.error, WIRELESS_ERROR_WRONG_PASSWORD);
    CHECK(!wireless_simulator_authorized());
    CHECK_EQ(fake_wireless_addressing_started(), 0u);
    int length = 0;
    fake_wireless_storage(&length);
    CHECK_EQ(length, -1);
}

TEST(wireless_manager, a_password_too_short_to_be_one_never_reaches_the_network) {
    start(0);
    steps(2);
    request("Workshop", "short");
    steps(2);
    CHECK_EQ(status().state, WIRELESS_STATE_FAILED);
    CHECK_EQ(status().error, WIRELESS_ERROR_BAD_PASSWORD_FORMAT);
}

TEST(wireless_manager, an_open_network_joins_without_a_handshake) {
    start(0);
    steps(2);
    request("Corner Cafe", "");
    steps(3);
    CHECK_EQ(status().state, WIRELESS_STATE_CONNECTED);
    CHECK(wireless_simulator_authorized());
}

TEST(wireless_manager, a_wpa3_only_network_is_refused_before_anything_is_sent) {
    start(0);
    steps(2);
    request("Laboratory", "anything at all");
    steps(2);
    CHECK_EQ(status().state, WIRELESS_STATE_FAILED);
    CHECK_EQ(status().error, WIRELESS_ERROR_UNSUPPORTED_SECURITY);
}

TEST(wireless_manager, a_name_nobody_answers_to_is_not_found) {
    start(0);
    steps(2);
    request("Nowhere", "whatever it is");
    steps(2);
    CHECK_EQ(status().state, WIRELESS_STATE_FAILED);
    CHECK_EQ(status().error, WIRELESS_ERROR_NOT_FOUND);
}

TEST(wireless_manager, a_remembered_network_is_rejoined_after_a_restart_with_no_password) {
    start(0);
    steps(2);
    request("Workshop", "lean os wireless");
    steps(3);
    REQUIRE(status().state == WIRELESS_STATE_CONNECTED);

    start(1);
    steps(3);
    os_wireless_status_t s = status();
    CHECK_EQ(s.state, WIRELESS_STATE_CONNECTED);
    CHECK_EQ(strcmp(s.ssid, "Workshop"), 0);
    CHECK(wireless_simulator_authorized());
}

TEST(wireless_manager, a_remembered_network_joins_from_the_list_with_no_password) {
    start(0);
    steps(2);
    request("Workshop", "lean os wireless");
    steps(3);
    REQUIRE(wireless_request_disconnect() == 0);
    steps(2);
    REQUIRE(status().state == WIRELESS_STATE_IDLE);
    request("Workshop", "");
    steps(3);
    CHECK_EQ(status().state, WIRELESS_STATE_CONNECTED);
}

TEST(wireless_manager, disconnecting_stays_disconnected) {
    start(0);
    steps(2);
    request("Workshop", "lean os wireless");
    steps(3);
    REQUIRE(wireless_request_disconnect() == 0);
    steps(2);
    CHECK_EQ(status().state, WIRELESS_STATE_IDLE);
    CHECK(!wireless_simulator_authorized());
    now += 120000;
    steps(5);
    CHECK_EQ(status().state, WIRELESS_STATE_IDLE);
}

TEST(wireless_manager, forgetting_leaves_the_network_and_the_file) {
    start(0);
    steps(2);
    request("Workshop", "lean os wireless");
    steps(3);
    REQUIRE(wireless_request_forget("Workshop", 8) == 0);
    steps(2);
    CHECK_EQ(status().state, WIRELESS_STATE_IDLE);
    int length = -1;
    fake_wireless_storage(&length);
    CHECK_EQ(length, 0);
    start(1);
    steps(3);
    CHECK_EQ(status().state, WIRELESS_STATE_IDLE);
}

TEST(wireless_manager, a_network_with_no_dhcp_answer_is_left) {
    start(0);
    steps(2);
    fake_wireless_set_addressing(-1, 1);
    request("Corner Cafe", "");
    steps(3);
    CHECK_EQ(status().state, WIRELESS_STATE_FAILED);
    CHECK_EQ(status().error, WIRELESS_ERROR_NO_ADDRESS);
    CHECK(!wireless_simulator_authorized());
}

static uint32_t arp_request(uint8_t *frame) {
    static const uint8_t station[6] = {0x02, 0x4C, 0x45, 0x41, 0x4E, 0x01};
    memset(frame, 0, 42);
    memset(frame, 0xFF, 6);
    memcpy(frame + 6, station, 6);
    frame[12] = 0x08;
    frame[13] = 0x06;
    uint8_t *arp = frame + 14;
    arp[1] = 1;
    arp[2] = 0x08;
    arp[4] = 6;
    arp[5] = 4;
    arp[7] = 1;
    memcpy(arp + 8, station, 6);
    arp[14] = 192;
    arp[15] = 168;
    arp[16] = 77;
    arp[17] = 23;
    arp[24] = 192;
    arp[25] = 168;
    arp[26] = 77;
    arp[27] = 1;
    return 42;
}

TEST(wireless_manager, ip_frames_cross_the_link_both_ways_once_joined) {
    start(0);
    steps(2);
    uint8_t frame[64];
    uint32_t length = arp_request(frame);
    REQUIRE(wireless_link_send(frame, (uint16_t)length) == 0);
    steps(2);
    CHECK_EQ(fake_wireless_delivered_count(), 0u);

    request("Workshop", "lean os wireless");
    steps(3);
    REQUIRE(status().state == WIRELESS_STATE_CONNECTED);
    REQUIRE(wireless_link_send(frame, (uint16_t)length) == 0);
    steps(2);
    REQUIRE(fake_wireless_delivered_count() == 1);
    uint32_t reply_length = 0;
    const uint8_t *reply = fake_wireless_delivered(0, &reply_length);
    REQUIRE(reply_length >= 42);
    CHECK_EQ(reply[12], 0x08);
    CHECK_EQ(reply[13], 0x06);
    CHECK_EQ(reply[14 + 7], 2);
    CHECK_EQ(memcmp(reply, frame + 6, 6), 0);
}

TEST(wireless_manager, a_transmit_ring_that_is_full_refuses_rather_than_overwrites) {
    start(0);
    steps(2);
    uint8_t frame[64];
    uint32_t length = arp_request(frame);
    int refused = 0;
    for (int i = 0; i < 40; i++) {
        if (wireless_link_send(frame, (uint16_t)length) != 0) {
            refused++;
        }
    }
    CHECK_EQ(refused, 8);
}

TEST(wireless_manager, a_wired_link_keeps_ip_and_the_radio_still_joins) {
    start(0);
    steps(2);
    fake_wireless_set_addressing(0, 0);
    request("Corner Cafe", "");
    steps(2);
    CHECK_EQ(status().state, WIRELESS_STATE_CONNECTED);
    CHECK_EQ(status().ip, 0u);
}

TEST(wireless_manager, a_network_joined_without_remembering_is_not_written_down) {
    start(0);
    steps(2);
    os_wireless_connect_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.ssid, "Workshop", 8);
    r.ssid_length = 8;
    snprintf(r.password, sizeof(r.password), "lean os wireless");
    r.remember = 0;
    REQUIRE(wireless_request_connect(&r) == 0);
    steps(3);
    CHECK_EQ(status().state, WIRELESS_STATE_CONNECTED);
    int length = 0;
    fake_wireless_storage(&length);
    CHECK_EQ(length, -1);
}

TEST(wireless_manager, a_machine_with_no_radio_says_so_and_refuses_requests) {
    os_wireless_status_t s;
    wireless_status(&s);
    if (s.state == WIRELESS_STATE_ABSENT) {
        CHECK_EQ(wireless_request_scan(), -19);
    }
    start(0);
    steps(1);
    os_wireless_connect_t r;
    memset(&r, 0, sizeof(r));
    CHECK_EQ(wireless_request_connect(&r), -22);
    r.ssid_length = WIRELESS_SSID_MAX + 1;
    CHECK_EQ(wireless_request_connect(&r), -22);
    CHECK_EQ(wireless_request_forget("x", 0), -22);
    CHECK_EQ(wireless_request_forget("x", WIRELESS_SSID_MAX + 1), -22);
    CHECK_EQ(wireless_request_scan(), 0);
    CHECK_EQ(status().state, WIRELESS_STATE_SCANNING);
    steps(1);
    CHECK_EQ(status().state, WIRELESS_STATE_IDLE);
}

TEST(wireless_manager, forgetting_another_network_keeps_this_one) {
    start(0);
    steps(2);
    request("Corner Cafe", "");
    steps(3);
    REQUIRE(status().state == WIRELESS_STATE_CONNECTED);
    request("Workshop", "lean os wireless");
    steps(3);
    REQUIRE(status().state == WIRELESS_STATE_CONNECTED);
    REQUIRE(wireless_request_forget("Corner Cafe", 11) == 0);
    steps(2);
    CHECK_EQ(status().state, WIRELESS_STATE_CONNECTED);
    CHECK_EQ(strcmp(status().ssid, "Workshop"), 0);
    os_wireless_network_t rows[WIRELESS_NETWORKS_MAX];
    long count = wireless_networks(rows, WIRELESS_NETWORKS_MAX);
    int cafe = find_row(rows, count, "Corner Cafe");
    REQUIRE(cafe >= 0);
    CHECK(!rows[cafe].known);
    CHECK(rows[find_row(rows, count, "Workshop")].known);
}

TEST(wireless_manager, a_link_frame_too_long_for_a_radio_is_refused) {
    start(0);
    steps(1);
    static uint8_t frame[2000];
    CHECK(wireless_link_send(frame, 1601) != 0);
    CHECK_EQ(wireless_link_send(frame, 1600), 0);
}

/* The file as somebody else might have written it: upper-case digits, a
   line with half a key, a line with no key, a name that is not hexadecimal.
   Only whole entries count; the rest are not guessed at. */
TEST(wireless_manager, the_remembered_file_is_read_strictly) {
    uint8_t pmk[32];
    REQUIRE(wpa_passphrase_to_pmk("lean os wireless", (const uint8_t *)"Workshop", 8, pmk));
    char key[65];
    for (int i = 0; i < 32; i++) {
        snprintf(key + 2 * i, 3, "%02X", pmk[i]);
    }
    char text[512];
    snprintf(text, sizeof(text),
             "436F726E65722043616665 0011\n"
             "4E65696768626F757273\n"
             "zz %s\n"
             "576F726B73686F70 %s\n",
             key, key);
    fake_wireless_glue_reset(0);
    fake_wireless_set_storage(text);
    wireless_manager_reset(wireless_simulator_backend());
    now = 1000;
    steps(3);
    os_wireless_network_t rows[WIRELESS_NETWORKS_MAX];
    long count = wireless_networks(rows, WIRELESS_NETWORKS_MAX);
    CHECK(rows[find_row(rows, count, "Workshop")].known);
    CHECK(!rows[find_row(rows, count, "Corner Cafe")].known);
    CHECK(!rows[find_row(rows, count, "Neighbours")].known);
    CHECK_EQ(status().state, WIRELESS_STATE_CONNECTED);
    CHECK_EQ(strcmp(status().ssid, "Workshop"), 0);
    CHECK(wireless_simulator_authorized());
}
