#include "ieee80211.h"

#define HEADER_LENGTH 24
#define FIXED_FIELDS  12

#define ELEMENT_SSID       0
#define ELEMENT_DS_PARAMETER 3
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
