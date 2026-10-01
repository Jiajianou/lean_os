#pragma once

#include <stddef.h>
#include <stdint.h>

/* The supplicant's half of WPA2-Personal: the four-way handshake that turns
   the pairwise master key both sides already hold into this session's keys,
   and the group-key handshake the access point repeats whenever it changes
   the key for broadcast traffic. It takes EAPOL-Key frames in and hands back
   the frame to send, plus the keys to install; the radio does the rest.

   Only AES-CCMP with HMAC-SHA1 (key descriptor version 2) is spoken, which
   is what every WPA2-Personal network offers. A network that requires
   protected management frames or SHA-256 key derivation is refused rather
   than half-joined. */

#define EAPOL_ETHERTYPE 0x888E

#define WPA_KEY_LENGTH 16
#define WPA_NONCE_LENGTH 32
#define WPA_GTK_MAX 32

enum {
    WPA_HANDSHAKE_IDLE = 0,
    WPA_HANDSHAKE_SENT_2_OF_4,
    WPA_HANDSHAKE_COMPLETE,
};

enum {
    WPA_RESULT_IGNORED = 0,
    WPA_RESULT_REPLY = 1,
    WPA_RESULT_INSTALL_PAIRWISE = 2,
    WPA_RESULT_INSTALL_GROUP = 4,
    WPA_RESULT_COMPLETE = 8,
    WPA_RESULT_BAD_MIC = 16,
    WPA_RESULT_REFUSED = 32,
};

typedef struct {
    uint8_t pmk[32];
    uint8_t own_address[6];
    uint8_t ap_address[6];
    uint8_t own_rsn[64];
    uint8_t own_rsn_length;
    uint8_t ap_rsn[64];
    uint8_t ap_rsn_length;

    uint8_t anonce[WPA_NONCE_LENGTH];
    uint8_t snonce[WPA_NONCE_LENGTH];
    uint8_t kck[WPA_KEY_LENGTH];
    uint8_t kek[WPA_KEY_LENGTH];
    uint8_t tk[WPA_KEY_LENGTH];
    int have_ptk;

    uint8_t replay_counter[8];
    int have_replay_counter;

    uint8_t gtk[WPA_GTK_MAX];
    uint8_t gtk_length;
    uint8_t gtk_index;
    uint8_t group_rsc[8];

    int state;
} wpa_handshake_t;

/* Our own RSN element for WPA2-PSK with CCMP, the one the association
   request carries and message 2 repeats. Returns its length. */
uint32_t wpa_own_rsn_element(uint8_t *out, uint32_t capacity);

/* Starts a handshake. `snonce` is this side's random number for it. */
void wpa_handshake_begin(wpa_handshake_t *handshake, const uint8_t pmk[32], const uint8_t own_address[6],
                         const uint8_t ap_address[6], const uint8_t *ap_rsn, uint32_t ap_rsn_length,
                         const uint8_t snonce[WPA_NONCE_LENGTH]);

/* One received EAPOL frame (from the 802.1X header on). Writes the frame to
   send back, if any, into `reply` and returns a set of WPA_RESULT_ bits. */
int wpa_handshake_receive(wpa_handshake_t *handshake, const uint8_t *frame, uint32_t length, uint8_t *reply,
                          uint32_t reply_capacity, uint32_t *reply_length);

/* The key a PTK derivation produces, exposed for the test against IEEE
   802.11's own test vector. */
void wpa_derive_ptk(const uint8_t pmk[32], const uint8_t a1[6], const uint8_t a2[6], const uint8_t n1[32],
                    const uint8_t n2[32], uint8_t kck[16], uint8_t kek[16], uint8_t tk[16]);
