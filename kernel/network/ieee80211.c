#include "ieee80211.h"

#define HEADER_LENGTH 24
#define FIXED_FIELDS  12

#define ELEMENT_SSID       0
#define ELEMENT_SUPPORTED_RATES 1
#define ELEMENT_DS_PARAMETER 3
#define ELEMENT_TIM        5
#define ELEMENT_EXTENDED_RATES 50
#define ELEMENT_RSN        48
#define ELEMENT_VENDOR     221

#define SUITE_TKIP 2
#define SUITE_CCMP 4
#define SUITE_GCMP 8

#define AKM_8021X       1
#define AKM_PSK         2
#define AKM_FT_8021X    3
#define AKM_FT_PSK      4
#define AKM_8021X_SHA256 5
#define AKM_PSK_SHA256  6
#define AKM_SAE         8
#define AKM_FT_SAE      9

static const uint8_t ieee_oui[3] = {0x00, 0x0F, 0xAC};
static const uint8_t microsoft_oui[3] = {0x00, 0x50, 0xF2};

static uint16_t read16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static int is_oui(const uint8_t *bytes, const uint8_t *oui) {
    return bytes[0] == oui[0] && bytes[1] == oui[1] && bytes[2] == oui[2];
}

static uint8_t cipher_bit(uint8_t type) {
    switch (type) {
        case SUITE_TKIP:
            return IEEE80211_CIPHER_TKIP;
        case SUITE_CCMP:
            return IEEE80211_CIPHER_CCMP;
        case SUITE_GCMP:
            return IEEE80211_CIPHER_GCMP;
        default:
            return 0;
    }
}

/* The RSN element and Microsoft's older WPA element have one shape after
   their headers: a group cipher, a list of pairwise ciphers, a list of key
   management suites. Either may stop early, and the standard says what an
   omitted field defaults to - CCMP for RSN, TKIP for WPA, and 802.1X for the
   key management of both. */
static void parse_suites(const uint8_t *body, uint32_t length, const uint8_t *oui, int wpa2,
                         ieee80211_network_t *out) {
    uint32_t at = 2;
    uint8_t group = wpa2 ? IEEE80211_CIPHER_CCMP : IEEE80211_CIPHER_TKIP;
    uint8_t pairwise = group;
    uint8_t security = IEEE80211_SECURITY_ENTERPRISE;

    if (length >= at + 4 && is_oui(body + at, oui)) {
        group = cipher_bit(body[at + 3]);
    }
    if (length >= at + 4) {
        at += 4;
    }
    if (length >= at + 2) {
        uint16_t count = read16(body + at);
        at += 2;
        pairwise = 0;
        for (uint16_t i = 0; i < count && length >= at + 4; i++, at += 4) {
            if (is_oui(body + at, oui)) {
                pairwise |= cipher_bit(body[at + 3]);
            }
        }
    }
    if (length >= at + 2) {
        uint16_t count = read16(body + at);
        at += 2;
        security = 0;
        for (uint16_t i = 0; i < count && length >= at + 4; i++, at += 4) {
            if (!is_oui(body + at, oui)) {
                continue;
            }
            uint8_t akm = body[at + 3];
            if (akm == AKM_PSK || akm == AKM_PSK_SHA256 || akm == AKM_FT_PSK) {
                security |= wpa2 ? IEEE80211_SECURITY_WPA2_PSK : IEEE80211_SECURITY_WPA_PSK;
            } else if (wpa2 && (akm == AKM_SAE || akm == AKM_FT_SAE)) {
                security |= IEEE80211_SECURITY_WPA3_SAE;
            } else if (akm == AKM_8021X || akm == AKM_FT_8021X || akm == AKM_8021X_SHA256) {
                security |= IEEE80211_SECURITY_ENTERPRISE;
            }
        }
    }
    if (wpa2 && length >= at + 2) {
        out->rsn_capabilities = read16(body + at);
    }
    out->security |= security;
    out->pairwise_ciphers |= pairwise;
    if (wpa2 || !out->group_cipher) {
        out->group_cipher = group;
    }
}

int ieee80211_parse_beacon(const uint8_t *frame, uint32_t length, ieee80211_network_t *out) {
    if (!frame || !out || length < HEADER_LENGTH + FIXED_FIELDS) {
        return 0;
    }
    uint16_t frame_control = read16(frame);
    uint16_t type = frame_control & IEEE80211_FRAME_CONTROL_TYPE_MASK;
    uint16_t subtype = frame_control & IEEE80211_FRAME_CONTROL_SUBTYPE_MASK;
    if (type != IEEE80211_TYPE_MANAGEMENT ||
        (subtype != IEEE80211_SUBTYPE_BEACON && subtype != IEEE80211_SUBTYPE_PROBE_RESPONSE)) {
        return 0;
    }

    uint8_t *clear = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) {
        clear[i] = 0;
    }
    for (int i = 0; i < 6; i++) {
        out->bssid[i] = frame[16 + i];
    }
    for (int i = 7; i >= 0; i--) {
        out->timestamp = out->timestamp << 8 | frame[HEADER_LENGTH + i];
    }
    out->beacon_interval = read16(frame + HEADER_LENGTH + 8);
    out->capability = read16(frame + HEADER_LENGTH + 10);

    int saw_ssid = 0;
    uint32_t at = HEADER_LENGTH + FIXED_FIELDS;
    while (at + 2 <= length) {
        uint8_t id = frame[at];
        uint8_t element_length = frame[at + 1];
        const uint8_t *body = frame + at + 2;
        if (at + 2 + element_length > length) {
            break;
        }
        if (id == ELEMENT_SSID && !saw_ssid) {
            saw_ssid = 1;
            uint8_t n = element_length > IEEE80211_SSID_MAX ? IEEE80211_SSID_MAX : element_length;
            int all_zero = 1;
            for (uint8_t i = 0; i < n; i++) {
                out->ssid[i] = (char)body[i];
                if (body[i] != 0) {
                    all_zero = 0;
                }
            }
            out->ssid[n] = 0;
            out->ssid_length = n;
            out->hidden = (n == 0 || all_zero) ? 1 : 0;
        } else if (id == ELEMENT_DS_PARAMETER && element_length >= 1) {
            out->channel = body[0];
        } else if (id == ELEMENT_TIM && element_length >= 2) {
            out->dtim_count = body[0];
            out->dtim_period = body[1];
        } else if (id == ELEMENT_RSN && element_length >= 2 && read16(body) == 1) {
            parse_suites(body, element_length, ieee_oui, 1, out);
            uint32_t keep = (uint32_t)element_length + 2;
            if (keep <= sizeof(out->rsn_element)) {
                for (uint32_t i = 0; i < keep; i++) {
                    out->rsn_element[i] = frame[at + i];
                }
                out->rsn_element_length = (uint8_t)keep;
            }
        } else if (id == ELEMENT_VENDOR && element_length >= 6 && is_oui(body, microsoft_oui) && body[3] == 1 &&
                   read16(body + 4) == 1) {
            parse_suites(body + 4, element_length - 4u, microsoft_oui, 0, out);
        }
        at += 2u + element_length;
    }
    if (!saw_ssid) {
        out->hidden = 1;
    }
    if (out->security == 0) {
        out->security = (out->capability & IEEE80211_CAPABILITY_PRIVACY) ? IEEE80211_SECURITY_WEP
                                                                          : IEEE80211_SECURITY_OPEN;
    }
    return 1;
}

const char *ieee80211_security_name(uint8_t security) {
    if (security & IEEE80211_SECURITY_ENTERPRISE) {
        return "Enterprise";
    }
    if ((security & IEEE80211_SECURITY_WPA2_PSK) && (security & IEEE80211_SECURITY_WPA3_SAE)) {
        return "WPA2/WPA3";
    }
    if (security & IEEE80211_SECURITY_WPA3_SAE) {
        return "WPA3";
    }
    if (security & IEEE80211_SECURITY_WPA2_PSK) {
        return "WPA2";
    }
    if (security & IEEE80211_SECURITY_WPA_PSK) {
        return "WPA";
    }
    if (security & IEEE80211_SECURITY_WEP) {
        return "WEP";
    }
    return "Open";
}

uint8_t ieee80211_channel_from_frequency(uint32_t mhz) {
    if (mhz == 2484) {
        return 14;
    }
    if (mhz >= 2412 && mhz <= 2472) {
        return (uint8_t)((mhz - 2407) / 5);
    }
    if (mhz >= 5000 && mhz <= 5900) {
        return (uint8_t)((mhz - 5000) / 5);
    }
    return 0;
}

static void write16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void copy_address(uint8_t *to, const uint8_t *from) {
    for (int i = 0; i < 6; i++) {
        to[i] = from[i];
    }
}

static int same_address(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* Every frame this station sends gets the next number: the receiver uses it
   to throw away retransmissions it already has, so two frames must not
   share one. */
static uint16_t next_sequence;

static void management_header(uint8_t *out, uint16_t subtype, const uint8_t own[6], const uint8_t bssid[6]) {
    write16(out, (uint16_t)(IEEE80211_TYPE_MANAGEMENT | subtype));
    write16(out + 2, 0);
    copy_address(out + 4, bssid);
    copy_address(out + 10, own);
    copy_address(out + 16, bssid);
    write16(out + 22, (uint16_t)(next_sequence++ << 4));
}

uint32_t ieee80211_build_authentication(uint8_t *out, const uint8_t own[6], const uint8_t bssid[6]) {
    management_header(out, IEEE80211_SUBTYPE_AUTHENTICATION, own, bssid);
    write16(out + HEADER_LENGTH, 0);
    write16(out + HEADER_LENGTH + 2, 1);
    write16(out + HEADER_LENGTH + 4, 0);
    return HEADER_LENGTH + 6;
}

uint32_t ieee80211_build_deauthentication(uint8_t *out, const uint8_t own[6], const uint8_t bssid[6],
                                          uint16_t reason) {
    management_header(out, IEEE80211_SUBTYPE_DEAUTHENTICATION, own, bssid);
    write16(out + HEADER_LENGTH, reason);
    return HEADER_LENGTH + 2;
}

/* Every legacy rate, in units of 500 kbit/s. None is marked basic: that bit
   is the access point's to set, and a station claiming one says nothing. */
static const uint8_t rates_24[] = {0x02, 0x04, 0x0B, 0x16, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C};
static const uint8_t rates_5[] = {0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C};

uint32_t ieee80211_build_association_request(uint8_t *out, uint32_t capacity, const uint8_t own[6],
                                             const ieee80211_network_t *network, const uint8_t *rsn,
                                             uint32_t rsn_length) {
    if (capacity < HEADER_LENGTH + 4 + 2 + IEEE80211_SSID_MAX + 2 + 8 + 2 + 4 + rsn_length) {
        return 0;
    }
    management_header(out, IEEE80211_SUBTYPE_ASSOCIATION_REQUEST, own, network->bssid);
    int low_band = network->channel <= 14;
    uint16_t capability = IEEE80211_CAPABILITY_ESS;
    if (network->capability & IEEE80211_CAPABILITY_PRIVACY) {
        capability |= IEEE80211_CAPABILITY_PRIVACY;
    }
    if (low_band) {
        capability |= IEEE80211_CAPABILITY_SHORT_SLOT;
    }
    write16(out + HEADER_LENGTH, capability);
    write16(out + HEADER_LENGTH + 2, 10);
    uint32_t at = HEADER_LENGTH + 4;
    out[at++] = ELEMENT_SSID;
    out[at++] = network->ssid_length;
    for (uint32_t i = 0; i < network->ssid_length; i++) {
        out[at++] = (uint8_t)network->ssid[i];
    }
    const uint8_t *rates = low_band ? rates_24 : rates_5;
    uint32_t count = low_band ? sizeof(rates_24) : sizeof(rates_5);
    uint32_t first = count > 8 ? 8 : count;
    out[at++] = ELEMENT_SUPPORTED_RATES;
    out[at++] = (uint8_t)first;
    for (uint32_t i = 0; i < first; i++) {
        out[at++] = rates[i];
    }
    if (count > first) {
        out[at++] = ELEMENT_EXTENDED_RATES;
        out[at++] = (uint8_t)(count - first);
        for (uint32_t i = first; i < count; i++) {
            out[at++] = rates[i];
        }
    }
    for (uint32_t i = 0; i < rsn_length; i++) {
        out[at++] = rsn[i];
    }
    return at;
}

static int management_from(const uint8_t *frame, uint32_t length, uint16_t subtype, const uint8_t own[6],
                           const uint8_t bssid[6]) {
    if (length < HEADER_LENGTH) {
        return 0;
    }
    uint16_t frame_control = read16(frame);
    return (frame_control & IEEE80211_FRAME_CONTROL_TYPE_MASK) == IEEE80211_TYPE_MANAGEMENT &&
           (frame_control & IEEE80211_FRAME_CONTROL_SUBTYPE_MASK) == subtype && same_address(frame + 4, own) &&
           same_address(frame + 10, bssid);
}

int ieee80211_parse_authentication(const uint8_t *frame, uint32_t length, const uint8_t own[6],
                                   const uint8_t bssid[6], uint16_t *sequence, uint16_t *status) {
    if (!management_from(frame, length, IEEE80211_SUBTYPE_AUTHENTICATION, own, bssid) ||
        length < HEADER_LENGTH + 6) {
        return 0;
    }
    *sequence = read16(frame + HEADER_LENGTH + 2);
    *status = read16(frame + HEADER_LENGTH + 4);
    return 1;
}

int ieee80211_parse_association_response(const uint8_t *frame, uint32_t length, const uint8_t own[6],
                                         const uint8_t bssid[6], uint16_t *status, uint16_t *association_id) {
    if (!management_from(frame, length, IEEE80211_SUBTYPE_ASSOCIATION_RESPONSE, own, bssid) ||
        length < HEADER_LENGTH + 6) {
        return 0;
    }
    *status = read16(frame + HEADER_LENGTH + 2);
    *association_id = (uint16_t)(read16(frame + HEADER_LENGTH + 4) & 0x3FFF);
    return 1;
}

int ieee80211_parse_departure(const uint8_t *frame, uint32_t length, const uint8_t own[6], const uint8_t bssid[6],
                              uint16_t *reason) {
    if (!(management_from(frame, length, IEEE80211_SUBTYPE_DEAUTHENTICATION, own, bssid) ||
          management_from(frame, length, IEEE80211_SUBTYPE_DISASSOCIATION, own, bssid)) ||
        length < HEADER_LENGTH + 2) {
        return 0;
    }
    *reason = read16(frame + HEADER_LENGTH);
    return 1;
}

static const uint8_t rfc1042[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};

#define ETHERNET_HEADER 14

uint32_t ieee80211_data_from_ethernet(const uint8_t *ethernet, uint32_t length, const uint8_t bssid[6], uint8_t *out,
                                      uint32_t capacity) {
    if (length < ETHERNET_HEADER || capacity < HEADER_LENGTH + 8 + (length - ETHERNET_HEADER)) {
        return 0;
    }
    write16(out, (uint16_t)(IEEE80211_TYPE_DATA | IEEE80211_FRAME_CONTROL_TO_DS));
    write16(out + 2, 0);
    copy_address(out + 4, bssid);
    copy_address(out + 10, ethernet + 6);
    copy_address(out + 16, ethernet);
    write16(out + 22, (uint16_t)(next_sequence++ << 4));
    for (int i = 0; i < 6; i++) {
        out[HEADER_LENGTH + i] = rfc1042[i];
    }
    out[HEADER_LENGTH + 6] = ethernet[12];
    out[HEADER_LENGTH + 7] = ethernet[13];
    uint32_t payload = length - ETHERNET_HEADER;
    for (uint32_t i = 0; i < payload; i++) {
        out[HEADER_LENGTH + 8 + i] = ethernet[ETHERNET_HEADER + i];
    }
    return HEADER_LENGTH + 8 + payload;
}

uint32_t ieee80211_header_length(const uint8_t *frame) {
    uint16_t frame_control = read16(frame);
    if ((frame_control & IEEE80211_FRAME_CONTROL_TYPE_MASK) == IEEE80211_TYPE_DATA &&
        (frame_control & IEEE80211_SUBTYPE_QOS_DATA)) {
        return HEADER_LENGTH + 2;
    }
    return HEADER_LENGTH;
}

uint32_t ieee80211_data_to_ethernet(const uint8_t *frame, uint32_t length, uint32_t payload_offset,
                                    const uint8_t bssid[6], uint8_t *out, uint32_t capacity) {
    if (length < HEADER_LENGTH || payload_offset < HEADER_LENGTH || length < payload_offset + 8) {
        return 0;
    }
    uint16_t frame_control = read16(frame);
    uint16_t subtype = frame_control & IEEE80211_FRAME_CONTROL_SUBTYPE_MASK;
    if ((frame_control & IEEE80211_FRAME_CONTROL_TYPE_MASK) != IEEE80211_TYPE_DATA ||
        (frame_control & (IEEE80211_FRAME_CONTROL_TO_DS | IEEE80211_FRAME_CONTROL_FROM_DS)) !=
            IEEE80211_FRAME_CONTROL_FROM_DS ||
        (subtype & 0x40u) || !same_address(frame + 10, bssid)) {
        return 0;
    }
    if (subtype & IEEE80211_SUBTYPE_QOS_DATA && (frame[HEADER_LENGTH] & 0x80u)) {
        return 0;
    }
    const uint8_t *payload = frame + payload_offset;
    for (int i = 0; i < 6; i++) {
        if (payload[i] != rfc1042[i]) {
            return 0;
        }
    }
    uint32_t body = length - payload_offset - 8;
    if (capacity < ETHERNET_HEADER + body) {
        return 0;
    }
    copy_address(out, frame + 4);
    copy_address(out + 6, frame + 16);
    out[12] = payload[6];
    out[13] = payload[7];
    for (uint32_t i = 0; i < body; i++) {
        out[ETHERNET_HEADER + i] = payload[8 + i];
    }
    return ETHERNET_HEADER + body;
}
