#pragma once

#include <stdint.h>

/* IEEE 802.11 frames as a station sees them: the beacons and probe
   responses a scan collects, and what they say about a network - its name,
   its channel, and what it takes to join it. */

#define IEEE80211_FRAME_CONTROL_TYPE_MASK    0x000Cu
#define IEEE80211_FRAME_CONTROL_SUBTYPE_MASK 0x00F0u
#define IEEE80211_TYPE_MANAGEMENT            0x0000u
#define IEEE80211_TYPE_DATA                  0x0008u
#define IEEE80211_SUBTYPE_BEACON             0x0080u
#define IEEE80211_SUBTYPE_PROBE_RESPONSE     0x0050u
#define IEEE80211_SUBTYPE_ASSOCIATION_REQUEST  0x0000u
#define IEEE80211_SUBTYPE_ASSOCIATION_RESPONSE 0x0010u
#define IEEE80211_SUBTYPE_DISASSOCIATION     0x00A0u
#define IEEE80211_SUBTYPE_AUTHENTICATION     0x00B0u
#define IEEE80211_SUBTYPE_DEAUTHENTICATION   0x00C0u
#define IEEE80211_SUBTYPE_QOS_DATA           0x0080u

#define IEEE80211_FRAME_CONTROL_TO_DS     0x0100u
#define IEEE80211_FRAME_CONTROL_FROM_DS   0x0200u
#define IEEE80211_FRAME_CONTROL_PROTECTED 0x4000u

#define IEEE80211_HEADER_LENGTH 24
#define IEEE80211_CCMP_HEADER_LENGTH 8

#define IEEE80211_CAPABILITY_ESS          0x0001u
#define IEEE80211_CAPABILITY_PRIVACY      0x0010u
#define IEEE80211_CAPABILITY_SHORT_PREAMBLE 0x0020u
#define IEEE80211_CAPABILITY_SHORT_SLOT   0x0400u

#define IEEE80211_STATUS_SUCCESS 0

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
    /* What the firmware needs to wake for this network's beacons once it has
       joined: the beacon's own timestamp, and where in the DTIM cycle that
       beacon was. */
    uint64_t timestamp;
    uint8_t dtim_count;
    uint8_t dtim_period;
} ieee80211_network_t;

/* A beacon or probe response, from its frame control field on. Returns 0
   when the frame is not one, or is too short to say anything. */
int ieee80211_parse_beacon(const uint8_t *frame, uint32_t length, ieee80211_network_t *out);

/* "WPA2", "WPA3", "WPA2/WPA3", "Open"... for a picker to show. */
const char *ieee80211_security_name(uint8_t security);

/* The 802.11 channel a frequency in MHz is, or 0. */
uint8_t ieee80211_channel_from_frequency(uint32_t mhz);

/* The frames a station sends to join a network, each written into `out` from
   its frame control field on. They return the frame's length. Joining is
   open-system authentication followed by an association request that offers
   every legacy rate the band has and, when `rsn` is given, the RSN element
   saying which security this station chose. */
uint32_t ieee80211_build_authentication(uint8_t *out, const uint8_t own[6], const uint8_t bssid[6]);
uint32_t ieee80211_build_association_request(uint8_t *out, uint32_t capacity, const uint8_t own[6],
                                             const ieee80211_network_t *network, const uint8_t *rsn,
                                             uint32_t rsn_length);
uint32_t ieee80211_build_deauthentication(uint8_t *out, const uint8_t own[6], const uint8_t bssid[6],
                                          uint16_t reason);

/* A management frame addressed to `own` from `bssid`, taken apart. Returns
   0 when the frame is not one of these from that network. */
int ieee80211_parse_authentication(const uint8_t *frame, uint32_t length, const uint8_t own[6],
                                   const uint8_t bssid[6], uint16_t *sequence, uint16_t *status);
int ieee80211_parse_association_response(const uint8_t *frame, uint32_t length, const uint8_t own[6],
                                         const uint8_t bssid[6], uint16_t *status, uint16_t *association_id);
/* A deauthentication or disassociation from the network: the reason. */
int ieee80211_parse_departure(const uint8_t *frame, uint32_t length, const uint8_t own[6], const uint8_t bssid[6],
                              uint16_t *reason);

/* An Ethernet frame (destination, source, type, payload) as the 802.11 data
   frame a station sends its access point: To-DS, the network in address 1,
   and the payload behind an RFC 1042 header. Returns 0 when it does not fit. */
uint32_t ieee80211_data_from_ethernet(const uint8_t *ethernet, uint32_t length, const uint8_t bssid[6], uint8_t *out,
                                      uint32_t capacity);

/* The other direction: a data frame from the access point with the 802.11
   header at `frame` and the payload `payload_offset` bytes in (past whatever
   security header or padding the radio left), back into an Ethernet frame.
   Returns 0 for a frame that is not plain data from that network to here. */
uint32_t ieee80211_data_to_ethernet(const uint8_t *frame, uint32_t length, uint32_t payload_offset,
                                    const uint8_t bssid[6], uint8_t *out, uint32_t capacity);

/* The length of a frame's 802.11 header: 24, or 26 for QoS data. */
uint32_t ieee80211_header_length(const uint8_t *frame);
