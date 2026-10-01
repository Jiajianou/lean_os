#pragma once

#include <stdint.h>

/* IEEE 802.11 frames as a station sees them: the beacons and probe
   responses a scan collects, and what they say about a network - its name,
   its channel, and what it takes to join it. */

#define IEEE80211_FRAME_CONTROL_TYPE_MASK    0x000Cu
#define IEEE80211_FRAME_CONTROL_SUBTYPE_MASK 0x00F0u
#define IEEE80211_TYPE_MANAGEMENT            0x0000u
#define IEEE80211_SUBTYPE_BEACON             0x0080u
#define IEEE80211_SUBTYPE_PROBE_RESPONSE     0x0050u

#define IEEE80211_CAPABILITY_PRIVACY 0x0010u

#define IEEE80211_SSID_MAX 32

/* What joining takes. A network can offer several at once - WPA2 and WPA3
   together is common - so this is a set. */
#define IEEE80211_SECURITY_OPEN       0x01u
#define IEEE80211_SECURITY_WEP        0x02u
#define IEEE80211_SECURITY_WPA_PSK    0x04u
#define IEEE80211_SECURITY_WPA2_PSK   0x08u
#define IEEE80211_SECURITY_WPA3_SAE   0x10u
#define IEEE80211_SECURITY_ENTERPRISE 0x20u

#define IEEE80211_CIPHER_TKIP 0x1u
#define IEEE80211_CIPHER_CCMP 0x2u
#define IEEE80211_CIPHER_GCMP 0x4u

#define IEEE80211_RSN_CAPABILITY_MFP_REQUIRED 0x0040u
#define IEEE80211_RSN_CAPABILITY_MFP_CAPABLE  0x0080u

typedef struct {
    uint8_t bssid[6];
    char ssid[IEEE80211_SSID_MAX + 1];
    uint8_t ssid_length;
    uint8_t hidden;
    uint8_t channel;
    uint16_t beacon_interval;
    uint16_t capability;
    uint8_t security;
    uint8_t pairwise_ciphers;
    uint8_t group_cipher;
    uint16_t rsn_capabilities;
    uint8_t rsn_element[64];
    uint8_t rsn_element_length;
} ieee80211_network_t;

/* A beacon or probe response, from its frame control field on. Returns 0
   when the frame is not one, or is too short to say anything. */
int ieee80211_parse_beacon(const uint8_t *frame, uint32_t length, ieee80211_network_t *out);

/* "WPA2", "WPA3", "WPA2/WPA3", "Open"... for a picker to show. */
const char *ieee80211_security_name(uint8_t security);

/* The 802.11 channel a frequency in MHz is, or 0. */
uint8_t ieee80211_channel_from_frequency(uint32_t mhz);
