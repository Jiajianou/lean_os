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
