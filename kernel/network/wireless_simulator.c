#include "network/wireless_simulator.h"

#include "drivers/kernel_log.h"
#include "network/ieee80211.h"
#include "network/wireless_manager.h"
#include "network/wpa_crypto.h"
#include "network/wpa_handshake.h"
#ifndef LEANOS_HOST_TEST
#include "device/fwcfg.h"
#include "scheduler/scheduler.h"
#endif

/* A radio for a machine that has none - QEMU emulates no wireless card - and
   on the other side of it, five access points. It exists so that everything
   above the driver can be run and graded where the harnesses run: the picker,
   the password, the handshake, remembering, DHCP and IP over the radio.

   The access points are not a stub that says yes. Each WPA2 one holds its own
   passphrase and runs the authenticator's half of the four-way handshake with
   the same cryptography the station uses: a wrong password produces a
   message 2 whose MIC does not verify, three tries, and a deauthentication -
   which is what a real access point does and what the station has to
   recognise. And the keys the station installs are compared with the ones the
   access point derived, so a handshake that completes with the wrong keys is
   a failure here as it would be silence on a real network.

   It is switched on from outside the image, by fw_cfg, the way the self-tests
   are - never by default, and never on a machine with a radio of its own. */

#define SIMULATED_AP_COUNT 5
#define ROUTER_IP 0xC0A84D01u
#define OFFERED_IP 0xC0A84D17u
#define SUBNET_MASK 0xFFFFFF00u
#define LEASE_SECONDS 3600u
#define HANDSHAKE_ATTEMPTS 3
#define REASON_HANDSHAKE_TIMEOUT 15

#define KIND_OPEN 0
#define KIND_WPA2 1
#define KIND_WPA3_ONLY 2

typedef struct {
    const char *ssid;
    uint8_t last_byte;
    int8_t signal;
    uint8_t channel;
    uint8_t kind;
    const char *password;
} access_point_t;

static const access_point_t access_points[SIMULATED_AP_COUNT] = {
    {"Workshop", 0x01, -45, 6, KIND_WPA2, "lean os wireless"},
    {"Workshop", 0x02, -63, 36, KIND_WPA2, "lean os wireless"},
    {"Corner Cafe", 0x03, -58, 11, KIND_OPEN, 0},
    {"Laboratory", 0x04, -71, 1, KIND_WPA3_ONLY, 0},
    {"Neighbours", 0x05, -82, 149, KIND_WPA2, "not yours to know"},
};

static const uint8_t station_address[6] = {0x02, 0x4C, 0x45, 0x41, 0x4E, 0x01};

static struct {
    int associated;
    const access_point_t *ap;
    uint8_t bssid[6];
    uint8_t pmk[32];
    uint8_t anonce[32];
    uint8_t kck[16];
    uint8_t kek[16];
    uint8_t tk[16];
    uint8_t gtk[16];
    uint64_t replay;
    int attempts;
    int authorized;
    int pairwise_installed;
    int group_installed;
    uint32_t frames_sent;
    uint32_t keys_wrong;
} sim;

static void copy(void *to, const void *from, uint32_t length) {
    uint8_t *t = (uint8_t *)to;
    const uint8_t *f = (const uint8_t *)from;
    for (uint32_t i = 0; i < length; i++) {
        t[i] = f[i];
    }
}

static void zero(void *pointer, uint32_t length) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (uint32_t i = 0; i < length; i++) {
        bytes[i] = 0;
    }
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static uint32_t text_length(const char *text) {
    uint32_t n = 0;
    while (text[n]) {
        n++;
    }
    return n;
}

static void bssid_of(const access_point_t *ap, uint8_t out[6]) {
    static const uint8_t prefix[5] = {0x02, 0x5A, 0x00, 0x00, 0x00};
    copy(out, prefix, 5);
    out[5] = ap->last_byte;
}

static void put16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put_be16(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static void put_be32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *at) {
    return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

static const uint8_t rsn_wpa2[] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F,
                                   0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0x0C, 0};
static const uint8_t rsn_wpa3[] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F,
                                   0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 8, 0xC0, 0};

/* A beacon, built as the bytes an access point would send and taken apart
   by the same parser a real scan uses. */
static uint32_t build_beacon(const access_point_t *ap, uint8_t *frame) {
    zero(frame, 128);
    frame[0] = 0x80;
    for (int i = 0; i < 6; i++) {
        frame[4 + i] = 0xFF;
    }
    bssid_of(ap, frame + 10);
    bssid_of(ap, frame + 16);
    put16(frame + 24 + 8, 100);
    put16(frame + 24 + 10, (uint16_t)(IEEE80211_CAPABILITY_ESS | (ap->kind != KIND_OPEN ? 0x10 : 0)));
    uint32_t at = 36;
    uint32_t name = text_length(ap->ssid);
    frame[at++] = 0;
    frame[at++] = (uint8_t)name;
    copy(frame + at, ap->ssid, name);
    at += name;
    frame[at++] = 3;
    frame[at++] = 1;
    frame[at++] = ap->channel;
    frame[at++] = 5;
    frame[at++] = 4;
    frame[at++] = 0;
    frame[at++] = 2;
    frame[at++] = 0;
    frame[at++] = 0;
    if (ap->kind == KIND_WPA2) {
        copy(frame + at, rsn_wpa2, sizeof(rsn_wpa2));
        at += sizeof(rsn_wpa2);
    } else if (ap->kind == KIND_WPA3_ONLY) {
        copy(frame + at, rsn_wpa3, sizeof(rsn_wpa3));
        at += sizeof(rsn_wpa3);
    }
    return at;
}

static int simulator_scan(void) {
    uint8_t frame[128];
    for (int i = 0; i < SIMULATED_AP_COUNT; i++) {
        uint32_t length = build_beacon(&access_points[i], frame);
        ieee80211_network_t network;
        if (ieee80211_parse_beacon(frame, length, &network)) {
            wireless_heard(&network, access_points[i].signal);
        }
    }
    return 0;
}

static void send_to_station(uint16_t type, const uint8_t *payload, uint32_t length, const uint8_t *destination) {
    static uint8_t frame[1600];
    if (length + 14 > sizeof(frame)) {
        return;
    }
    copy(frame, destination, 6);
    copy(frame + 6, sim.bssid, 6);
    put_be16(frame + 12, type);
    copy(frame + 14, payload, length);
    sim.frames_sent++;
    wireless_received(frame, 14 + length);
}

#define EAPOL_FIXED 99
#define INFO_VERSION 2u
#define INFO_PAIRWISE 0x0008u
#define INFO_INSTALL 0x0040u
#define INFO_ACK 0x0080u
#define INFO_MIC 0x0100u
#define INFO_SECURE 0x0200u
#define INFO_ENCRYPTED 0x1000u

static void eapol_mic(uint8_t *frame, uint32_t length, uint8_t mic[16]) {
    uint8_t saved[16];
    copy(saved, frame + 81, 16);
    zero(frame + 81, 16);
    uint8_t digest[20];
    hmac_sha1(sim.kck, 16, frame, length, digest);
    copy(frame + 81, saved, 16);
    copy(mic, digest, 16);
}

static void send_key_message(uint16_t information, const uint8_t *data, uint32_t data_length) {
    static uint8_t frame[EAPOL_FIXED + 128];
    uint32_t length = EAPOL_FIXED + data_length;
    zero(frame, sizeof(frame));
    frame[0] = 2;
    frame[1] = 3;
    put_be16(frame + 2, length - 4);
    frame[4] = 2;
    put_be16(frame + 5, information);
    put_be16(frame + 7, 16);
    sim.replay++;
    for (int i = 0; i < 8; i++) {
        frame[9 + i] = (uint8_t)(sim.replay >> (56 - 8 * i));
    }
    copy(frame + 17, sim.anonce, 32);
    put_be16(frame + 97, data_length);
    copy(frame + EAPOL_FIXED, data, data_length);
    if (information & INFO_MIC) {
        uint8_t mic[16];
        eapol_mic(frame, length, mic);
        copy(frame + 81, mic, 16);
    }
    send_to_station(EAPOL_ETHERTYPE, frame, length, station_address);
}

static void send_message_one(void) {
    send_key_message((uint16_t)(INFO_VERSION | INFO_PAIRWISE | INFO_ACK), 0, 0);
}

static void send_message_three(void) {
    uint8_t plain[64];
    uint32_t at = 0;
    copy(plain, rsn_wpa2, sizeof(rsn_wpa2));
    at += sizeof(rsn_wpa2);
    static const uint8_t kde[] = {0xDD, 22, 0x00, 0x0F, 0xAC, 0x01, 0x01, 0x00};
    copy(plain + at, kde, sizeof(kde));
    at += sizeof(kde);
    copy(plain + at, sim.gtk, 16);
    at += 16;
    if (at % 8 != 0) {
        plain[at++] = 0xDD;
        while (at % 8 != 0) {
            plain[at++] = 0;
        }
    }
    uint8_t wrapped[72];
    aes_key_wrap(sim.kek, plain, at, wrapped);
    send_key_message((uint16_t)(INFO_VERSION | INFO_PAIRWISE | INFO_INSTALL | INFO_ACK | INFO_MIC | INFO_SECURE |
                                INFO_ENCRYPTED),
                     wrapped, at + 8);
}

static void send_away(void) {
    sim.associated = 0;
    sim.authorized = 0;
    wireless_departed(REASON_HANDSHAKE_TIMEOUT);
}

static void authenticator_receive(const uint8_t *frame, uint32_t length) {
    if (length < EAPOL_FIXED || frame[1] != 3) {
        return;
    }
    uint16_t information = (uint16_t)(frame[5] << 8 | frame[6]);
    if (!(information & INFO_MIC) || (information & INFO_ACK)) {
        return;
    }
    static uint8_t copy_of[512];
    if (length > sizeof(copy_of)) {
        return;
    }
    if ((information & INFO_PAIRWISE) && !(information & INFO_SECURE)) {
        wpa_derive_ptk(sim.pmk, sim.bssid, station_address, sim.anonce, frame + 17, sim.kck, sim.kek, sim.tk);
        copy(copy_of, frame, length);
        uint8_t mic[16];
        eapol_mic(copy_of, length, mic);
        if (!same(mic, frame + 81, 16)) {
            kernel_log_puts("[wifi-sim] message 2's MIC does not verify - the station has another password\n");
            if (++sim.attempts >= HANDSHAKE_ATTEMPTS) {
                send_away();
                return;
            }
            send_message_one();
            return;
        }
        send_message_three();
        return;
    }
    if ((information & INFO_PAIRWISE) && (information & INFO_SECURE)) {
        copy(copy_of, frame, length);
        uint8_t mic[16];
        eapol_mic(copy_of, length, mic);
        if (same(mic, frame + 81, 16)) {
            sim.authorized = 1;
            kernel_log_puts("[wifi-sim] message 4 verified - the station is authorised\n");
        }
    }
}

static uint16_t checksum(const uint8_t *bytes, uint32_t length, uint32_t sum) {
    for (uint32_t i = 0; i + 1 < length; i += 2) {
        sum += (uint32_t)bytes[i] << 8 | bytes[i + 1];
    }
    if (length & 1) {
        sum += (uint32_t)bytes[length - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint32_t ip_header(uint8_t *packet, uint8_t protocol, uint32_t source, uint32_t destination,
                          uint32_t payload_length) {
    zero(packet, 20);
    packet[0] = 0x45;
    put_be16(packet + 2, 20 + payload_length);
    packet[8] = 64;
    packet[9] = protocol;
    put_be32(packet + 12, source);
    put_be32(packet + 16, destination);
    put_be16(packet + 10, checksum(packet, 20, 0));
    return 20;
}

static void answer_arp(const uint8_t *packet, uint32_t length, const uint8_t *from) {
    if (length < 28 || packet[7] != 1 || get_be32(packet + 24) != ROUTER_IP) {
        return;
    }
    uint8_t reply[28];
    copy(reply, packet, 8);
    reply[7] = 2;
    copy(reply + 8, sim.bssid, 6);
    put_be32(reply + 14, ROUTER_IP);
    copy(reply + 18, packet + 8, 6);
    copy(reply + 24, packet + 14, 4);
    send_to_station(0x0806, reply, sizeof(reply), from);
}

static const uint8_t *dhcp_option(const uint8_t *message, uint32_t length, uint8_t code) {
    uint32_t at = 240;
    while (at + 2 <= length && message[at] != 255) {
        if (message[at] == 0) {
            at++;
            continue;
        }
        if (message[at] == code) {
            return message + at;
        }
        at += 2u + message[at + 1];
    }
    return 0;
}

static void answer_dhcp(const uint8_t *udp, uint32_t length, const uint8_t *from) {
    const uint8_t *message = udp + 8;
    uint32_t message_length = length - 8;
    if (message_length < 240 || message[0] != 1) {
        return;
    }
    const uint8_t *type = dhcp_option(message, message_length, 53);
    if (!type || type[1] != 1 || (type[2] != 1 && type[2] != 3)) {
        return;
    }
    static uint8_t packet[20 + 8 + 300];
    zero(packet, sizeof(packet));
    uint8_t *reply = packet + 28;
    reply[0] = 2;
    reply[1] = 1;
    reply[2] = 6;
    copy(reply + 4, message + 4, 4);
    put_be32(reply + 16, OFFERED_IP);
    put_be32(reply + 20, ROUTER_IP);
    copy(reply + 28, message + 28, 16);
    put_be32(reply + 236, 0x63825363u);
    uint32_t at = 240;
    reply[at++] = 53;
    reply[at++] = 1;
    reply[at++] = type[2] == 1 ? 2 : 5;
    reply[at++] = 54;
    reply[at++] = 4;
    put_be32(reply + at, ROUTER_IP);
    at += 4;
    reply[at++] = 51;
    reply[at++] = 4;
    put_be32(reply + at, LEASE_SECONDS);
    at += 4;
    reply[at++] = 1;
    reply[at++] = 4;
    put_be32(reply + at, SUBNET_MASK);
    at += 4;
    reply[at++] = 3;
    reply[at++] = 4;
    put_be32(reply + at, ROUTER_IP);
    at += 4;
    reply[at++] = 6;
    reply[at++] = 4;
    put_be32(reply + at, ROUTER_IP);
    at += 4;
    reply[at++] = 255;
    while (at < 300) {
        reply[at++] = 0;
    }
    uint8_t *header = packet + 20;
    put_be16(header, 67);
    put_be16(header + 2, 68);
    put_be16(header + 4, 8 + at);
    ip_header(packet, 17, ROUTER_IP, 0xFFFFFFFFu, 8 + at);
    send_to_station(0x0800, packet, 28 + at, from);
}

static void answer_ping(const uint8_t *ip, uint32_t length, const uint8_t *from) {
    uint32_t header = (uint32_t)(ip[0] & 15) * 4;
    if (length < header + 8 || ip[header] != 8) {
        return;
    }
    static uint8_t packet[1500];
    uint32_t payload = length - header;
    if (payload + 20 > sizeof(packet)) {
        return;
    }
    copy(packet + 20, ip + header, payload);
    packet[20] = 0;
    packet[22] = 0;
    packet[23] = 0;
    put_be16(packet + 22, checksum(packet + 20, payload, 0));
    ip_header(packet, 1, ROUTER_IP, get_be32(ip + 12), payload);
    send_to_station(0x0800, packet, 20 + payload, from);
}

static void router_receive(const uint8_t *frame, uint32_t length) {
    uint16_t type = (uint16_t)(frame[12] << 8 | frame[13]);
    const uint8_t *payload = frame + 14;
    uint32_t payload_length = length - 14;
    if (type == 0x0806) {
        answer_arp(payload, payload_length, frame + 6);
        return;
    }
    if (type != 0x0800 || payload_length < 20) {
        return;
    }
    uint32_t header = (uint32_t)(payload[0] & 15) * 4;
    uint32_t total = (uint32_t)payload[2] << 8 | payload[3];
    if (total > payload_length || header < 20 || total < header) {
        return;
    }
    if (payload[9] == 17 && total >= header + 8 && payload[header + 2] == 0 && payload[header + 3] == 67) {
        answer_dhcp(payload + header, total - header, frame + 6);
    } else if (payload[9] == 1 && get_be32(payload + 16) == ROUTER_IP) {
        answer_ping(payload, total, frame + 6);
    }
}

static int simulator_join(const ieee80211_network_t *network, const uint8_t *rsn, uint32_t rsn_length,
                          uint16_t *status) {
    (void)rsn;
    sim.associated = 0;
    for (int i = 0; i < SIMULATED_AP_COUNT; i++) {
        uint8_t bssid[6];
        bssid_of(&access_points[i], bssid);
        if (!same(bssid, network->bssid, 6)) {
            continue;
        }
        const access_point_t *ap = &access_points[i];
        if (ap->kind == KIND_WPA3_ONLY || (ap->kind == KIND_WPA2) != (rsn_length > 0)) {
            *status = 43;
            return -1;
        }
        sim.ap = ap;
        copy(sim.bssid, bssid, 6);
        sim.associated = 1;
        sim.authorized = ap->kind == KIND_OPEN;
        sim.attempts = 0;
        sim.pairwise_installed = 0;
        sim.group_installed = 0;
        sim.replay = 0;
        *status = 0;
        if (ap->kind == KIND_WPA2) {
            wpa_passphrase_to_pmk(ap->password, (const uint8_t *)ap->ssid, text_length(ap->ssid), sim.pmk);
            wireless_random(sim.anonce, sizeof(sim.anonce));
            wireless_random(sim.gtk, sizeof(sim.gtk));
            send_message_one();
        }
        return 0;
    }
    *status = 0xFFFF;
    return -1;
}

static int simulator_transmit(const uint8_t *ethernet, uint32_t length) {
    if (!sim.associated || length < 14) {
        return -1;
    }
    uint16_t type = (uint16_t)(ethernet[12] << 8 | ethernet[13]);
    if (type == EAPOL_ETHERTYPE) {
        authenticator_receive(ethernet + 14, length - 14);
        return 0;
    }
    if (!sim.authorized || (sim.ap->kind == KIND_WPA2 && !sim.pairwise_installed)) {
        return 0;
    }
    router_receive(ethernet, length);
    return 0;
}

static int simulator_install_key(int group, const uint8_t *key, uint32_t length, uint8_t index) {
    const uint8_t *expected = group ? sim.gtk : sim.tk;
    if (!sim.associated || length != 16 || !same(key, expected, 16) || (group && index != 1)) {
        sim.keys_wrong++;
        kernel_log_puts("[wifi-sim] the station installed a key the access point does not hold\n");
        send_away();
        return -1;
    }
    if (group) {
        sim.group_installed = 1;
    } else {
        sim.pairwise_installed = 1;
    }
    return 0;
}

static void simulator_leave(void) {
    sim.associated = 0;
    sim.authorized = 0;
}

static int simulator_service(void) {
    return 0;
}

static wireless_backend_t simulator_backend = {
    "simulated",
    {0x02, 0x4C, 0x45, 0x41, 0x4E, 0x01},
    simulator_scan,
    simulator_join,
    simulator_transmit,
    simulator_install_key,
    simulator_leave,
    simulator_service,
};

const wireless_backend_t *wireless_simulator_backend(void) {
    zero(&sim, sizeof(sim));
    return &simulator_backend;
}

uint32_t wireless_simulator_keys_wrong(void) {
    return sim.keys_wrong;
}

int wireless_simulator_authorized(void) {
    return sim.authorized && (sim.ap && (sim.ap->kind == KIND_OPEN || (sim.pairwise_installed && sim.group_installed)));
}

#ifndef LEANOS_HOST_TEST
static void simulator_task(void *argument) {
    (void)argument;
    kernel_log_puts("[wifi] no radio on this machine - a simulated one, as fw_cfg asked\n");
    wireless_run(wireless_simulator_backend());
}

int wireless_simulator_start(void) {
    char want[16];
    int n = fwcfg_read_file("opt/leanos/wireless", want, sizeof(want) - 1);
    if (n < 9) {
        return 0;
    }
    want[n] = 0;
    const char *expected = "simulated";
    for (int i = 0; i < 9; i++) {
        if (want[i] != expected[i]) {
            return 0;
        }
    }
    task_t *task = task_spawn("wifi", simulator_task, 0);
    if (task) {
        task->parent_id = -1;
    }
    return 1;
}
#endif
