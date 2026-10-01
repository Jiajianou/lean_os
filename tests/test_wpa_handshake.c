#include "check.h"

#include "network/wpa_handshake.h"
#include "wpa/fixtures.h"

/* The access point in these tests is tests/wpa/make_fixtures.py - Python's
   hmac and the host's openssl, nothing shared with the code under test. Its
   frames go in and this side's answers have to come out byte for byte. */

static wpa_handshake_t handshake;
static uint8_t reply[512];
static uint32_t reply_length;

static void begin(void) {
    wpa_handshake_begin(&handshake, pmk, own_address, ap_address, ap_rsn, sizeof(ap_rsn), snonce);
}

static int receive(const uint8_t *frame, uint32_t length) {
    return wpa_handshake_receive(&handshake, frame, length, reply, sizeof(reply), &reply_length);
}

TEST(wpa_handshake, the_ptk_comes_out_of_the_prf_the_way_the_access_point_derives_it) {
    uint8_t kck[16], kek[16], tk[16];
    wpa_derive_ptk(pmk, own_address, ap_address, snonce, anonce, kck, kek, tk);
    CHECK_EQ(memcmp(kck, expected_kck, 16), 0);
    CHECK_EQ(memcmp(kek, expected_kek, 16), 0);
    CHECK_EQ(memcmp(tk, expected_tk, 16), 0);
    wpa_derive_ptk(pmk, ap_address, own_address, anonce, snonce, kck, kek, tk);
    CHECK_EQ(memcmp(tk, expected_tk, 16), 0);
}

TEST(wpa_handshake, four_messages_and_both_keys) {
    begin();
    CHECK_EQ(receive(message_1, sizeof(message_1)), WPA_RESULT_REPLY);
    REQUIRE(reply_length == sizeof(expected_message_2));
    CHECK_EQ(memcmp(reply, expected_message_2, sizeof(expected_message_2)), 0);

    int result = receive(message_3, sizeof(message_3));
    CHECK_EQ(result, WPA_RESULT_REPLY | WPA_RESULT_INSTALL_PAIRWISE | WPA_RESULT_INSTALL_GROUP | WPA_RESULT_COMPLETE);
    REQUIRE(reply_length == sizeof(expected_message_4));
    CHECK_EQ(memcmp(reply, expected_message_4, sizeof(expected_message_4)), 0);
    CHECK_EQ(memcmp(handshake.tk, expected_tk, 16), 0);
    CHECK_EQ(handshake.gtk_length, 16);
    CHECK_EQ(handshake.gtk_index, 1);
    CHECK_EQ(memcmp(handshake.gtk, gtk1, 16), 0);
    CHECK_EQ(handshake.group_rsc[0], 5);
    CHECK_EQ(handshake.state, WPA_HANDSHAKE_COMPLETE);
}

TEST(wpa_handshake, a_new_group_key_is_taken_and_acknowledged) {
    begin();
    receive(message_1, sizeof(message_1));
    receive(message_3, sizeof(message_3));
    int result = receive(group_message_1, sizeof(group_message_1));
    CHECK_EQ(result, WPA_RESULT_REPLY | WPA_RESULT_INSTALL_GROUP);
    REQUIRE(reply_length == sizeof(expected_group_message_2));
    CHECK_EQ(memcmp(reply, expected_group_message_2, sizeof(expected_group_message_2)), 0);
    CHECK_EQ(handshake.gtk_index, 2);
    CHECK_EQ(memcmp(handshake.gtk, gtk2, 16), 0);
}

TEST(wpa_handshake, a_replayed_message_3_is_ignored) {
    begin();
    receive(message_1, sizeof(message_1));
    receive(message_3, sizeof(message_3));
    CHECK_EQ(receive(message_3, sizeof(message_3)), WPA_RESULT_IGNORED);
    CHECK_EQ(reply_length, 0u);
}

TEST(wpa_handshake, a_wrong_passphrase_shows_as_a_bad_mic_on_message_3) {
    uint8_t wrong[32];
    memcpy(wrong, pmk, 32);
    wrong[0] ^= 1;
    wpa_handshake_begin(&handshake, wrong, own_address, ap_address, ap_rsn, sizeof(ap_rsn), snonce);
    CHECK_EQ(receive(message_1, sizeof(message_1)), WPA_RESULT_REPLY);
    CHECK_EQ(receive(message_3, sizeof(message_3)), WPA_RESULT_BAD_MIC);
    CHECK(handshake.state != WPA_HANDSHAKE_COMPLETE);
}

TEST(wpa_handshake, a_tampered_message_3_fails_its_mic) {
    begin();
    receive(message_1, sizeof(message_1));
    uint8_t tampered[sizeof(message_3)];
    memcpy(tampered, message_3, sizeof(tampered));
    tampered[sizeof(tampered) - 1] ^= 0x40;
    CHECK_EQ(receive(tampered, sizeof(tampered)), WPA_RESULT_BAD_MIC);
}

TEST(wpa_handshake, a_downgraded_rsn_element_in_message_3_is_refused) {
    begin();
    receive(message_1, sizeof(message_1));
    CHECK_EQ(receive(message_3_downgraded, sizeof(message_3_downgraded)), WPA_RESULT_REFUSED);
    CHECK(handshake.state != WPA_HANDSHAKE_COMPLETE);
}

TEST(wpa_handshake, message_3_before_message_1_is_ignored) {
    begin();
    CHECK_EQ(receive(message_3, sizeof(message_3)), WPA_RESULT_IGNORED);
    CHECK_EQ(receive(group_message_1, sizeof(group_message_1)), WPA_RESULT_IGNORED);
}

TEST(wpa_handshake, a_short_or_foreign_frame_is_ignored_and_never_overread) {
    begin();
    for (uint32_t cut = 0; cut < sizeof(message_1); cut++) {
        receive(message_1, cut);
    }
    uint8_t other[sizeof(message_1)];
    memcpy(other, message_1, sizeof(other));
    other[1] = 0;
    CHECK_EQ(receive(other, sizeof(other)), WPA_RESULT_IGNORED);
    memcpy(other, message_1, sizeof(other));
    other[6] = (uint8_t)((other[6] & ~7) | 1);
    CHECK_EQ(receive(other, sizeof(other)), WPA_RESULT_REFUSED);
}

TEST(wpa_handshake, our_rsn_element_is_wpa2_psk_with_ccmp) {
    uint8_t element[64];
    uint32_t length = wpa_own_rsn_element(element, sizeof(element));
    CHECK_EQ(length, sizeof(ap_rsn));
    CHECK_EQ(memcmp(element, ap_rsn, sizeof(ap_rsn)), 0);
}
