#include "check.h"

#include "network/ieee80211.h"

typedef struct {
    uint8_t bytes[512];
    uint32_t length;
} frame_t;

static void beacon(frame_t *f, uint16_t capability) {
    memset(f, 0, sizeof(*f));
    f->bytes[0] = 0x80;
    memset(f->bytes + 4, 0xFF, 6);
    const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(f->bytes + 10, bssid, 6);
    memcpy(f->bytes + 16, bssid, 6);
    f->bytes[24 + 8] = 100;
    f->bytes[24 + 10] = (uint8_t)capability;
    f->bytes[24 + 11] = (uint8_t)(capability >> 8);
    f->length = 36;
}

static void element(frame_t *f, uint8_t id, const uint8_t *body, uint8_t length) {
    f->bytes[f->length] = id;
    f->bytes[f->length + 1] = length;
    memcpy(f->bytes + f->length + 2, body, length);
    f->length += 2u + length;
}

static void ssid(frame_t *f, const char *name) {
    element(f, 0, (const uint8_t *)name, (uint8_t)strlen(name));
}

static void channel(frame_t *f, uint8_t number) {
    element(f, 3, &number, 1);
}

static const uint8_t rsn_wpa2_psk[] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                       1, 0, 0x00, 0x0F, 0xAC, 2, 0x0C, 0x00};

TEST(ieee80211, a_wpa2_personal_network_says_so) {
    frame_t f;
    beacon(&f, 0x0411);
    ssid(&f, "lean_os home");
    channel(&f, 6);
    element(&f, 48, rsn_wpa2_psk, sizeof(rsn_wpa2_psk));
    ieee80211_network_t n;
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK_EQ(strcmp(n.ssid, "lean_os home"), 0);
    CHECK_EQ(n.ssid_length, 12);
    CHECK_EQ(n.channel, 6);
    CHECK_EQ(n.beacon_interval, 100);
    CHECK_EQ(n.bssid[5], 0x55);
    CHECK_EQ(n.security, IEEE80211_SECURITY_WPA2_PSK);
    CHECK_EQ(n.pairwise_ciphers, IEEE80211_CIPHER_CCMP);
    CHECK_EQ(n.group_cipher, IEEE80211_CIPHER_CCMP);
    CHECK_EQ(n.rsn_capabilities, 0x000C);
    CHECK_EQ(n.rsn_element_length, sizeof(rsn_wpa2_psk) + 2);
    CHECK_EQ(n.rsn_element[0], 48);
    CHECK_EQ(strcmp(ieee80211_security_name(n.security), "WPA2"), 0);
    CHECK(!n.hidden);
}

TEST(ieee80211, a_transition_network_offers_both_wpa2_and_wpa3) {
    const uint8_t rsn[] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                           2, 0, 0x00, 0x0F, 0xAC, 2, 0x00, 0x0F, 0xAC, 8, 0x80, 0x00};
    frame_t f;
    beacon(&f, 0x0011);
    ssid(&f, "both");
    element(&f, 48, rsn, sizeof(rsn));
    ieee80211_network_t n;
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK_EQ(n.security, IEEE80211_SECURITY_WPA2_PSK | IEEE80211_SECURITY_WPA3_SAE);
    CHECK_EQ(n.rsn_capabilities & IEEE80211_RSN_CAPABILITY_MFP_CAPABLE, IEEE80211_RSN_CAPABILITY_MFP_CAPABLE);
    CHECK_EQ(strcmp(ieee80211_security_name(n.security), "WPA2/WPA3"), 0);
}

TEST(ieee80211, the_old_wpa_element_is_microsofts_and_defaults_to_tkip) {
    const uint8_t wpa[] = {0x00, 0x50, 0xF2, 1, 1, 0, 0x00, 0x50, 0xF2, 2, 1, 0,
                           0x00, 0x50, 0xF2, 2, 1, 0, 0x00, 0x50, 0xF2, 2};
    frame_t f;
    beacon(&f, 0x0011);
    ssid(&f, "old");
    element(&f, 221, wpa, sizeof(wpa));
    ieee80211_network_t n;
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK_EQ(n.security, IEEE80211_SECURITY_WPA_PSK);
    CHECK_EQ(n.pairwise_ciphers, IEEE80211_CIPHER_TKIP);
    CHECK_EQ(n.group_cipher, IEEE80211_CIPHER_TKIP);
    CHECK_EQ(strcmp(ieee80211_security_name(n.security), "WPA"), 0);
}

TEST(ieee80211, privacy_with_no_rsn_element_is_wep_and_without_it_open) {
    frame_t f;
    ieee80211_network_t n;
    beacon(&f, 0x0011);
    ssid(&f, "wep");
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK_EQ(n.security, IEEE80211_SECURITY_WEP);
    beacon(&f, 0x0001);
    ssid(&f, "cafe");
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK_EQ(n.security, IEEE80211_SECURITY_OPEN);
    CHECK_EQ(strcmp(ieee80211_security_name(n.security), "Open"), 0);
}

TEST(ieee80211, an_rsn_element_that_omits_its_suites_means_ccmp_and_8021x) {
    const uint8_t rsn[] = {1, 0};
    frame_t f;
    beacon(&f, 0x0011);
    ssid(&f, "bare");
    element(&f, 48, rsn, sizeof(rsn));
    ieee80211_network_t n;
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK_EQ(n.security, IEEE80211_SECURITY_ENTERPRISE);
    CHECK_EQ(n.group_cipher, IEEE80211_CIPHER_CCMP);
}

TEST(ieee80211, a_hidden_network_names_itself_with_nothing_or_zeros) {
    frame_t f;
    ieee80211_network_t n;
    beacon(&f, 0x0001);
    element(&f, 0, (const uint8_t *)"\0\0\0\0", 4);
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK(n.hidden);
    beacon(&f, 0x0001);
    element(&f, 0, (const uint8_t *)"", 0);
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK(n.hidden);
}

TEST(ieee80211, an_element_running_past_the_frame_is_not_read) {
    frame_t f;
    beacon(&f, 0x0001);
    ssid(&f, "edge");
    f.bytes[f.length] = 48;
    f.bytes[f.length + 1] = 200;
    f.length += 4;
    ieee80211_network_t n;
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK_EQ(n.security, IEEE80211_SECURITY_OPEN);
    for (uint32_t cut = 0; cut < f.length; cut++) {
        ieee80211_parse_beacon(f.bytes, cut, &n);
    }
}

TEST(ieee80211, only_beacons_and_probe_responses_are_networks) {
    frame_t f;
    ieee80211_network_t n;
    beacon(&f, 0x0001);
    ssid(&f, "x");
    f.bytes[0] = 0x50;
    CHECK_EQ(ieee80211_parse_beacon(f.bytes, f.length, &n), 1);
    f.bytes[0] = 0x40;
    CHECK_EQ(ieee80211_parse_beacon(f.bytes, f.length, &n), 0);
    f.bytes[0] = 0x88;
    CHECK_EQ(ieee80211_parse_beacon(f.bytes, f.length, &n), 0);
    CHECK_EQ(ieee80211_parse_beacon(f.bytes, 30, &n), 0);
}

TEST(ieee80211, frequencies_become_channels) {
    CHECK_EQ(ieee80211_channel_from_frequency(2412), 1);
    CHECK_EQ(ieee80211_channel_from_frequency(2437), 6);
    CHECK_EQ(ieee80211_channel_from_frequency(2484), 14);
    CHECK_EQ(ieee80211_channel_from_frequency(5180), 36);
    CHECK_EQ(ieee80211_channel_from_frequency(5825), 165);
    CHECK_EQ(ieee80211_channel_from_frequency(900), 0);
}

static const uint8_t own[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x01};
static const uint8_t network_address[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

TEST(ieee80211, a_beacon_carries_its_timestamp_and_dtim_place) {
    frame_t f;
    beacon(&f, 0x0011);
    for (int i = 0; i < 8; i++) {
        f.bytes[24 + i] = (uint8_t)(0x10 + i);
    }
    ssid(&f, "timed");
    const uint8_t tim[] = {2, 3, 0, 0};
    element(&f, 5, tim, sizeof(tim));
    ieee80211_network_t n;
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n) == 1);
    CHECK(n.timestamp == 0x1716151413121110ull);
    CHECK_EQ(n.dtim_count, 2);
    CHECK_EQ(n.dtim_period, 3);
    CHECK(!n.hidden);
}

TEST(ieee80211, wpa3_alone_is_sae_and_beside_wpa2_is_both) {
    static const uint8_t sae[] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                  1, 0, 0x00, 0x0F, 0xAC, 8, 0xC0, 0x00};
    static const uint8_t both[] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                   2, 0, 0x00, 0x0F, 0xAC, 2, 0x00, 0x0F, 0xAC, 8, 0x80, 0x00};
    static const uint8_t foreign[] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                      1, 0, 0x00, 0x50, 0xF2, 2, 0x00, 0x00};
    frame_t f;
    ieee80211_network_t n;
    beacon(&f, 0x0011);
    ssid(&f, "sae");
    element(&f, 48, sae, sizeof(sae));
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n));
    CHECK_EQ(n.security, IEEE80211_SECURITY_WPA3_SAE);
    CHECK(n.rsn_capabilities & IEEE80211_RSN_CAPABILITY_MFP_REQUIRED);
    beacon(&f, 0x0011);
    ssid(&f, "both");
    element(&f, 48, both, sizeof(both));
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n));
    CHECK_EQ(n.security, IEEE80211_SECURITY_WPA2_PSK | IEEE80211_SECURITY_WPA3_SAE);
    beacon(&f, 0x0011);
    ssid(&f, "foreign");
    element(&f, 48, foreign, sizeof(foreign));
    REQUIRE(ieee80211_parse_beacon(f.bytes, f.length, &n));
    CHECK_EQ(n.security, 0 + IEEE80211_SECURITY_WEP);
}

TEST(ieee80211, authentication_is_open_system_from_here_to_the_network) {
    uint8_t frame[64];
    memset(frame, 0xEE, sizeof(frame));
    uint32_t length = ieee80211_build_authentication(frame, own, network_address);
    REQUIRE(length == 30);
    CHECK_EQ(frame[0], 0xB0);
    CHECK_EQ(frame[1], 0);
    CHECK_EQ(memcmp(frame + 4, network_address, 6), 0);
    CHECK_EQ(memcmp(frame + 10, own, 6), 0);
    CHECK_EQ(memcmp(frame + 16, network_address, 6), 0);
    CHECK_EQ(frame[24], 0);
    CHECK_EQ(frame[25], 0);
    CHECK_EQ(frame[26], 1);
    CHECK_EQ(frame[27], 0);
    CHECK_EQ(frame[28], 0);
    CHECK_EQ(frame[29], 0);
    uint8_t second[64];
    ieee80211_build_authentication(second, own, network_address);
    CHECK((frame[22] | frame[23] << 8) != (second[22] | second[23] << 8));
}

TEST(ieee80211, an_association_request_offers_the_name_every_rate_and_the_rsn) {
    ieee80211_network_t n;
    memset(&n, 0, sizeof(n));
    memcpy(n.bssid, network_address, 6);
    memcpy(n.ssid, "home", 4);
    n.ssid_length = 4;
    n.channel = 6;
    n.capability = IEEE80211_CAPABILITY_ESS | IEEE80211_CAPABILITY_PRIVACY;
    const uint8_t rsn[] = {48, 2, 1, 0};
    uint8_t frame[256];
    uint32_t length = ieee80211_build_association_request(frame, sizeof(frame), own, &n, rsn, sizeof(rsn));
    const uint8_t expected[] = {0x11, 0x04, 10, 0, 0, 4, 'h', 'o', 'm', 'e', 1, 8, 0x02, 0x04, 0x0B, 0x16,
                                0x0C, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6C, 48, 2, 1, 0};
    REQUIRE(length == 24 + sizeof(expected));
    CHECK_EQ(frame[0], 0x00);
    CHECK_EQ(memcmp(frame + 4, network_address, 6), 0);
    CHECK_EQ(memcmp(frame + 10, own, 6), 0);
    CHECK_EQ(memcmp(frame + 24, expected, sizeof(expected)), 0);

    n.channel = 36;
    n.capability = IEEE80211_CAPABILITY_ESS;
    length = ieee80211_build_association_request(frame, sizeof(frame), own, &n, 0, 0);
    const uint8_t five[] = {0x01, 0x00, 10, 0, 0, 4, 'h', 'o', 'm', 'e', 1, 8,
                            0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C};
    REQUIRE(length == 24 + sizeof(five));
    CHECK_EQ(memcmp(frame + 24, five, sizeof(five)), 0);
    CHECK_EQ(ieee80211_build_association_request(frame, 40, own, &n, 0, 0), 0u);
}

static uint32_t management(uint8_t *frame, uint8_t subtype, const uint8_t *to, const uint8_t *from,
                           const uint8_t *body, uint32_t body_length) {
    memset(frame, 0, 24);
    frame[0] = subtype;
    memcpy(frame + 4, to, 6);
    memcpy(frame + 10, from, 6);
    memcpy(frame + 16, from, 6);
    memcpy(frame + 24, body, body_length);
    return 24 + body_length;
}

TEST(ieee80211, answers_are_taken_only_from_the_network_and_only_to_here) {
    uint8_t frame[64];
    uint16_t a = 0, b = 0;
    const uint8_t auth[] = {0, 0, 2, 0, 17, 0};
    uint32_t length = management(frame, 0xB0, own, network_address, auth, sizeof(auth));
    REQUIRE(ieee80211_parse_authentication(frame, length, own, network_address, &a, &b));
    CHECK_EQ(a, 2);
    CHECK_EQ(b, 17);
    CHECK(!ieee80211_parse_authentication(frame, length - 1, own, network_address, &a, &b));
    CHECK(!ieee80211_parse_authentication(frame, length, network_address, network_address, &a, &b));
    CHECK(!ieee80211_parse_authentication(frame, length, own, own, &a, &b));
    CHECK(!ieee80211_parse_association_response(frame, length, own, network_address, &a, &b));

    const uint8_t response[] = {0x01, 0x04, 0, 0, 0x2A, 0xC0};
    length = management(frame, 0x10, own, network_address, response, sizeof(response));
    REQUIRE(ieee80211_parse_association_response(frame, length, own, network_address, &a, &b));
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 0x2A);
    CHECK(!ieee80211_parse_association_response(frame, length - 1, own, network_address, &a, &b));

    const uint8_t reason[] = {7, 0};
    length = management(frame, 0xC0, own, network_address, reason, sizeof(reason));
    REQUIRE(ieee80211_parse_departure(frame, length, own, network_address, &a));
    CHECK_EQ(a, 7);
    length = management(frame, 0xA0, own, network_address, reason, sizeof(reason));
    CHECK(ieee80211_parse_departure(frame, length, own, network_address, &a));
    CHECK(!ieee80211_parse_departure(frame, length - 1, own, network_address, &a));
    length = management(frame, 0xB0, own, network_address, reason, sizeof(reason));
    CHECK(!ieee80211_parse_departure(frame, length, own, network_address, &a));

    uint8_t deauth[32];
    CHECK_EQ(ieee80211_build_deauthentication(deauth, own, network_address, 3), 26u);
    CHECK_EQ(deauth[0], 0xC0);
    CHECK_EQ(deauth[24], 3);
    CHECK_EQ(memcmp(deauth + 4, network_address, 6), 0);
}

TEST(ieee80211, only_plain_data_from_the_network_becomes_ethernet) {
    uint8_t frame[128];
    uint8_t out[128];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08;
    frame[1] = 0x02;
    memcpy(frame + 4, own, 6);
    memcpy(frame + 10, network_address, 6);
    frame[16] = 0x77;
    const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x08, 0x00};
    memcpy(frame + 24, snap, 8);
    frame[32] = 0x45;
    uint32_t length = 24 + 8 + 20;
    REQUIRE(ieee80211_data_to_ethernet(frame, length, 24, network_address, out, sizeof(out)) == 14 + 20);
    CHECK_EQ(memcmp(out, own, 6), 0);
    CHECK_EQ(out[6], 0x77);
    CHECK_EQ(out[12], 0x08);
    CHECK_EQ(out[14], 0x45);
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length, 24, network_address, out, 20), 0u);
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length, 24, own, out, sizeof(out)), 0u);
    CHECK_EQ(ieee80211_data_to_ethernet(frame, 31, 24, network_address, out, sizeof(out)), 0u);
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length, 20, network_address, out, sizeof(out)), 0u);
    frame[1] = 0x01;
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length, 24, network_address, out, sizeof(out)), 0u);
    frame[1] = 0x02;
    frame[24] = 0xAB;
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length, 24, network_address, out, sizeof(out)), 0u);
    frame[24] = 0xAA;
    frame[0] = 0x48;
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length, 24, network_address, out, sizeof(out)), 0u);
    frame[0] = 0x00;
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length, 24, network_address, out, sizeof(out)), 0u);

    memmove(frame + 26, frame + 24, length - 24);
    frame[0] = 0x88;
    frame[24] = 0x80;
    frame[25] = 0;
    CHECK_EQ(ieee80211_header_length(frame), 26u);
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length + 2, 26, network_address, out, sizeof(out)), 0u);
    frame[24] = 0;
    CHECK_EQ(ieee80211_data_to_ethernet(frame, length + 2, 26, network_address, out, sizeof(out)), 14u + 20u);
}

TEST(ieee80211, ethernet_becomes_data_to_the_network) {
    uint8_t ethernet[34];
    memset(ethernet, 0, sizeof(ethernet));
    memset(ethernet, 0x66, 6);
    memcpy(ethernet + 6, own, 6);
    ethernet[12] = 0x08;
    ethernet[13] = 0x06;
    ethernet[14] = 0x99;
    ethernet[33] = 0x42;
    uint8_t out[128];
    uint32_t length = ieee80211_data_from_ethernet(ethernet, sizeof(ethernet), network_address, out, sizeof(out));
    REQUIRE(length == 24 + 8 + 20);
    CHECK_EQ(out[0], 0x08);
    CHECK_EQ(out[1], 0x01);
    CHECK_EQ(memcmp(out + 4, network_address, 6), 0);
    CHECK_EQ(memcmp(out + 10, own, 6), 0);
    CHECK_EQ(out[16], 0x66);
    const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x08, 0x06};
    CHECK_EQ(memcmp(out + 24, snap, 8), 0);
    CHECK_EQ(out[32], 0x99);
    CHECK_EQ(out[51], 0x42);
    CHECK_EQ(ieee80211_header_length(out), 24u);
    CHECK_EQ(ieee80211_data_from_ethernet(ethernet, 13, network_address, out, sizeof(out)), 0u);
    CHECK_EQ(ieee80211_data_from_ethernet(ethernet, sizeof(ethernet), network_address, out, 51), 0u);
}

TEST(ieee80211, channels_from_frequencies) {
    CHECK_EQ(ieee80211_channel_from_frequency(2412), 1);
    CHECK_EQ(ieee80211_channel_from_frequency(2484), 14);
    CHECK_EQ(ieee80211_channel_from_frequency(5180), 36);
    CHECK_EQ(ieee80211_channel_from_frequency(5805), 161);
    CHECK_EQ(ieee80211_channel_from_frequency(3000), 0);
}
