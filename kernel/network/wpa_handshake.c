#include "wpa_handshake.h"

#include "network/wpa_crypto.h"

#define EAPOL_TYPE_KEY 3
#define DESCRIPTOR_RSN 2

#define OFFSET_VERSION 0
#define OFFSET_TYPE 1
#define OFFSET_BODY_LENGTH 2
#define OFFSET_DESCRIPTOR 4
#define OFFSET_INFORMATION 5
#define OFFSET_KEY_LENGTH 7
#define OFFSET_REPLAY 9
#define OFFSET_NONCE 17
#define OFFSET_IV 49
#define OFFSET_RSC 65
#define OFFSET_MIC 81
#define OFFSET_DATA_LENGTH 97
#define FIXED_LENGTH 99

#define INFO_VERSION_MASK 0x0007u
#define INFO_VERSION_AES_HMAC_SHA1 2u
#define INFO_PAIRWISE 0x0008u
#define INFO_INSTALL 0x0040u
#define INFO_ACK 0x0080u
#define INFO_MIC 0x0100u
#define INFO_SECURE 0x0200u
#define INFO_ENCRYPTED 0x1000u

#define ELEMENT_RSN 48
#define ELEMENT_VENDOR 221
#define KDE_GTK 1

static void copy(void *to, const void *from, uint32_t length) {
    uint8_t *t = (uint8_t *)to;
    const uint8_t *f = (const uint8_t *)from;
    for (uint32_t i = 0; i < length; i++) {
        t[i] = f[i];
    }
}

static void wipe(void *pointer, uint32_t length) {
    volatile uint8_t *bytes = (volatile uint8_t *)pointer;
    for (uint32_t i = 0; i < length; i++) {
        bytes[i] = 0;
    }
}

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)((bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static int compare(const uint8_t *a, const uint8_t *b, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

uint32_t wpa_own_rsn_element(uint8_t *out, uint32_t capacity) {
    static const uint8_t element[] = {ELEMENT_RSN, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F,
                                      0xAC,        4,  1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
    if (capacity < sizeof(element)) {
        return 0;
    }
    copy(out, element, sizeof(element));
    return sizeof(element);
}

/* PTK = PRF-384(PMK, "Pairwise key expansion", min(AA, SPA) || max(AA, SPA)
   || min(ANonce, SNonce) || max(ANonce, SNonce)), cut into the key that
   signs handshake frames, the key that encrypts their key data, and the key
   the radio encrypts traffic with. */
void wpa_derive_ptk(const uint8_t pmk[32], const uint8_t a1[6], const uint8_t a2[6], const uint8_t n1[32],
                    const uint8_t n2[32], uint8_t kck[16], uint8_t kek[16], uint8_t tk[16]) {
    uint8_t data[6 + 6 + 32 + 32];
    int addresses = compare(a1, a2, 6) < 0;
    copy(data, addresses ? a1 : a2, 6);
    copy(data + 6, addresses ? a2 : a1, 6);
    int nonces = compare(n1, n2, 32) < 0;
    copy(data + 12, nonces ? n1 : n2, 32);
    copy(data + 44, nonces ? n2 : n1, 32);
    uint8_t ptk[48];
    wpa_prf(pmk, 32, "Pairwise key expansion", data, sizeof(data), ptk, sizeof(ptk));
    copy(kck, ptk, 16);
    copy(kek, ptk + 16, 16);
    copy(tk, ptk + 32, 16);
    wipe(ptk, sizeof(ptk));
}

void wpa_handshake_begin(wpa_handshake_t *handshake, const uint8_t pmk[32], const uint8_t own_address[6],
                         const uint8_t ap_address[6], const uint8_t *ap_rsn, uint32_t ap_rsn_length,
                         const uint8_t snonce[WPA_NONCE_LENGTH]) {
    wipe(handshake, sizeof(*handshake));
    copy(handshake->pmk, pmk, 32);
    copy(handshake->own_address, own_address, 6);
    copy(handshake->ap_address, ap_address, 6);
    if (ap_rsn && ap_rsn_length <= sizeof(handshake->ap_rsn)) {
        copy(handshake->ap_rsn, ap_rsn, ap_rsn_length);
        handshake->ap_rsn_length = (uint8_t)ap_rsn_length;
    }
    handshake->own_rsn_length = (uint8_t)wpa_own_rsn_element(handshake->own_rsn, sizeof(handshake->own_rsn));
    copy(handshake->snonce, snonce, WPA_NONCE_LENGTH);
    handshake->state = WPA_HANDSHAKE_IDLE;
}

static void compute_mic(const uint8_t kck[16], uint8_t *frame, uint32_t length, uint8_t mic[16]) {
    uint8_t saved[16];
    copy(saved, frame + OFFSET_MIC, 16);
    wipe(frame + OFFSET_MIC, 16);
    uint8_t digest[20];
    hmac_sha1(kck, 16, frame, length, digest);
    copy(frame + OFFSET_MIC, saved, 16);
    copy(mic, digest, 16);
    wipe(digest, sizeof(digest));
}

static int mic_valid(const uint8_t kck[16], const uint8_t *frame, uint32_t length) {
    static uint8_t scratch[2048];
    if (length > sizeof(scratch)) {
        return 0;
    }
    copy(scratch, frame, length);
    uint8_t mic[16];
    compute_mic(kck, scratch, length, mic);
    return wpa_equal(mic, frame + OFFSET_MIC, 16);
}

/* A replay counter only ever goes up; a frame that does not move it forward
   is a recording of an old one. */
static int replay_is_new(const wpa_handshake_t *handshake, const uint8_t *counter) {
    if (!handshake->have_replay_counter) {
        return 1;
    }
    return compare(counter, handshake->replay_counter, 8) > 0;
}

static uint32_t build_reply(const wpa_handshake_t *handshake, uint8_t version, uint16_t information,
                            const uint8_t *replay, const uint8_t *nonce, const uint8_t *data, uint32_t data_length,
                            uint8_t *reply, uint32_t capacity) {
    uint32_t length = FIXED_LENGTH + data_length;
    if (length > capacity) {
        return 0;
    }
    wipe(reply, length);
    reply[OFFSET_VERSION] = version;
    reply[OFFSET_TYPE] = EAPOL_TYPE_KEY;
    write_be16(reply + OFFSET_BODY_LENGTH, length - 4);
    reply[OFFSET_DESCRIPTOR] = DESCRIPTOR_RSN;
    write_be16(reply + OFFSET_INFORMATION, information);
    write_be16(reply + OFFSET_KEY_LENGTH, 0);
    copy(reply + OFFSET_REPLAY, replay, 8);
    if (nonce) {
        copy(reply + OFFSET_NONCE, nonce, 32);
    }
    write_be16(reply + OFFSET_DATA_LENGTH, data_length);
    if (data_length) {
        copy(reply + FIXED_LENGTH, data, data_length);
    }
    uint8_t mic[16];
    compute_mic(handshake->kck, reply, length, mic);
    copy(reply + OFFSET_MIC, mic, 16);
    return length;
}

/* The key data of message 3 and of a group message: the access point's RSN
   element again (which must be the one its beacon carried, or somebody is
   steering this station to weaker security), and the group key wrapped in a
   key data encapsulation. */
static int read_key_data(wpa_handshake_t *handshake, const uint8_t *data, uint32_t length, int check_rsn) {
    int saw_gtk = 0;
    int rsn_matches = handshake->ap_rsn_length == 0;
    uint32_t at = 0;
    while (at + 2 <= length) {
        uint8_t id = data[at];
        uint8_t element_length = data[at + 1];
        if (id == ELEMENT_VENDOR && element_length == 0) {
            break;
        }
        if (at + 2 + element_length > length) {
            break;
        }
        const uint8_t *body = data + at + 2;
        if (id == ELEMENT_RSN && handshake->ap_rsn_length) {
            rsn_matches = (uint32_t)element_length + 2 == handshake->ap_rsn_length &&
                          compare(data + at, handshake->ap_rsn, handshake->ap_rsn_length) == 0;
        } else if (id == ELEMENT_VENDOR && element_length >= 6 && body[0] == 0x00 && body[1] == 0x0F &&
                   body[2] == 0xAC && body[3] == KDE_GTK) {
            uint32_t key_length = element_length - 6u;
            if (key_length == 0 || key_length > WPA_GTK_MAX) {
                return 0;
            }
            handshake->gtk_index = body[4] & 0x3;
            handshake->gtk_length = (uint8_t)key_length;
            copy(handshake->gtk, body + 6, key_length);
            saw_gtk = 1;
        }
        at += 2u + element_length;
    }
    if (check_rsn && !rsn_matches) {
        return 0;
    }
    return saw_gtk;
}

static int unwrap_key_data(const wpa_handshake_t *handshake, const uint8_t *data, uint32_t length, uint8_t *out,
                           uint32_t *out_length) {
    if (length < 24 || length % 8 != 0 || length - 8 > 256) {
        return 0;
    }
    if (!aes_key_unwrap(handshake->kek, data, length, out)) {
        return 0;
    }
    *out_length = length - 8;
    return 1;
}

int wpa_handshake_receive(wpa_handshake_t *handshake, const uint8_t *frame, uint32_t length, uint8_t *reply,
                          uint32_t reply_capacity, uint32_t *reply_length) {
    *reply_length = 0;
    if (length < FIXED_LENGTH || frame[OFFSET_TYPE] != EAPOL_TYPE_KEY || frame[OFFSET_DESCRIPTOR] != DESCRIPTOR_RSN) {
        return WPA_RESULT_IGNORED;
    }
    uint32_t body = read_be16(frame + OFFSET_BODY_LENGTH);
    if (body + 4 > length) {
        return WPA_RESULT_IGNORED;
    }
    length = body + 4;
    uint16_t information = read_be16(frame + OFFSET_INFORMATION);
    uint32_t data_length = read_be16(frame + OFFSET_DATA_LENGTH);
    if (FIXED_LENGTH + data_length > length) {
        return WPA_RESULT_IGNORED;
    }
    if ((information & INFO_VERSION_MASK) != INFO_VERSION_AES_HMAC_SHA1) {
        return WPA_RESULT_REFUSED;
    }
    if (!(information & INFO_ACK)) {
        return WPA_RESULT_IGNORED;
    }
    const uint8_t *replay = frame + OFFSET_REPLAY;
    if (!replay_is_new(handshake, replay)) {
        return WPA_RESULT_IGNORED;
    }
    uint8_t version = frame[OFFSET_VERSION];
    const uint8_t *data = frame + FIXED_LENGTH;

    if ((information & INFO_PAIRWISE) && !(information & INFO_MIC)) {
        copy(handshake->anonce, frame + OFFSET_NONCE, WPA_NONCE_LENGTH);
        wpa_derive_ptk(handshake->pmk, handshake->own_address, handshake->ap_address, handshake->snonce,
                       handshake->anonce, handshake->kck, handshake->kek, handshake->tk);
        handshake->have_ptk = 1;
        copy(handshake->replay_counter, replay, 8);
        handshake->have_replay_counter = 1;
        *reply_length = build_reply(handshake, version, (uint16_t)(INFO_VERSION_AES_HMAC_SHA1 | INFO_PAIRWISE | INFO_MIC),
                                    replay, handshake->snonce, handshake->own_rsn, handshake->own_rsn_length, reply,
                                    reply_capacity);
        handshake->state = WPA_HANDSHAKE_SENT_2_OF_4;
        return *reply_length ? WPA_RESULT_REPLY : WPA_RESULT_IGNORED;
    }

    if (!handshake->have_ptk || !(information & INFO_MIC)) {
        return WPA_RESULT_IGNORED;
    }
    if (!mic_valid(handshake->kck, frame, length)) {
        return WPA_RESULT_BAD_MIC;
    }
    uint8_t plain[256];
    uint32_t plain_length = 0;

    if (information & INFO_PAIRWISE) {
        if (handshake->state != WPA_HANDSHAKE_SENT_2_OF_4 ||
            compare(frame + OFFSET_NONCE, handshake->anonce, WPA_NONCE_LENGTH) != 0 ||
            !(information & INFO_INSTALL) || !(information & INFO_ENCRYPTED)) {
            return WPA_RESULT_IGNORED;
        }
        if (!unwrap_key_data(handshake, data, data_length, plain, &plain_length) ||
            !read_key_data(handshake, plain, plain_length, 1)) {
            wipe(plain, sizeof(plain));
            return WPA_RESULT_REFUSED;
        }
        wipe(plain, sizeof(plain));
        copy(handshake->replay_counter, replay, 8);
        copy(handshake->group_rsc, frame + OFFSET_RSC, 8);
        *reply_length = build_reply(handshake, version,
                                    (uint16_t)(INFO_VERSION_AES_HMAC_SHA1 | INFO_PAIRWISE | INFO_MIC | INFO_SECURE),
                                    replay, 0, 0, 0, reply, reply_capacity);
        handshake->state = WPA_HANDSHAKE_COMPLETE;
        return WPA_RESULT_REPLY | WPA_RESULT_INSTALL_PAIRWISE | WPA_RESULT_INSTALL_GROUP | WPA_RESULT_COMPLETE;
    }

    if (handshake->state != WPA_HANDSHAKE_COMPLETE || !(information & INFO_ENCRYPTED)) {
        return WPA_RESULT_IGNORED;
    }
    if (!unwrap_key_data(handshake, data, data_length, plain, &plain_length) ||
        !read_key_data(handshake, plain, plain_length, 0)) {
        wipe(plain, sizeof(plain));
        return WPA_RESULT_REFUSED;
    }
    wipe(plain, sizeof(plain));
    copy(handshake->replay_counter, replay, 8);
    copy(handshake->group_rsc, frame + OFFSET_RSC, 8);
    *reply_length = build_reply(handshake, version, (uint16_t)(INFO_VERSION_AES_HMAC_SHA1 | INFO_MIC | INFO_SECURE),
                                replay, 0, 0, 0, reply, reply_capacity);
    return WPA_RESULT_REPLY | WPA_RESULT_INSTALL_GROUP;
}
