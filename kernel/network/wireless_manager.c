#include "wireless_manager.h"

#include "drivers/kernel_log.h"
#include "library/spinlock.h"
#include "network/wpa_crypto.h"
#include "network/wpa_handshake.h"

#define RESULTS_MAX 48
#define RING_SLOTS 32
#define FRAME_MAX 1600

#define SECURING_TIMEOUT_MS 6000
#define ADDRESSING_TIMEOUT_MS 20000
#define AUTOJOIN_RETRY_MS 30000
#define AUTOSCAN_MS 60000
#define REJOIN_AFTER_DEPARTURE_MS 5000

#define ETHERNET_HEADER 14

enum {
    PHASE_IDLE = 0,
    PHASE_SECURING,
    PHASE_ADDRESSING,
    PHASE_CONNECTED,
};

typedef struct {
    char ssid[WIRELESS_SSID_MAX + 1];
    uint8_t ssid_length;
    uint8_t pmk[32];
} known_network_t;

typedef struct {
    uint8_t bytes[FRAME_MAX];
    uint32_t length;
} frame_t;

/* Outside the structure a reset wipes: a system call on another processor
   may be holding one of them when the radio's task starts over. */
static spinlock_t state_lock;
static spinlock_t transmit_lock;

typedef struct {
    const wireless_backend_t *backend;

    int32_t state;
    int32_t error;
    char ssid[WIRELESS_SSID_MAX + 1];
    uint8_t ssid_length;
    uint32_t ip;
    uint32_t scan_generation;
    int8_t signal;
    uint8_t channel;
    os_wireless_network_t rows[WIRELESS_NETWORKS_MAX];
    uint32_t row_count;
    int want_scan;
    int want_disconnect;
    int want_connect;
    os_wireless_connect_t connect_request;
    int want_forget;
    char forget_ssid[WIRELESS_SSID_MAX + 1];
    uint8_t forget_length;

    frame_t transmit[RING_SLOTS];
    uint32_t transmit_head;
    uint32_t transmit_tail;
    uint32_t transmit_dropped;

    frame_t receive[RING_SLOTS];
    uint32_t receive_head;
    uint32_t receive_tail;

    ieee80211_network_t heard[RESULTS_MAX];
    int8_t heard_signal[RESULTS_MAX];
    uint32_t heard_count;
    ieee80211_network_t results[RESULTS_MAX];
    int8_t result_signal[RESULTS_MAX];
    uint32_t result_count;
    uint64_t last_scan_ms;

    known_network_t known[WIRELESS_KNOWN_MAX];
    uint32_t known_count;
    int known_loaded;

    int phase;
    uint64_t deadline_ms;
    ieee80211_network_t network;
    wpa_handshake_t handshake;
    int secured;
    int sent_message_two;
    int departed;
    uint16_t departure_reason;
    uint8_t pending_pmk[32];
    int remember_after_join;
    int autojoin_enabled;
    uint64_t next_autojoin_ms;
} manager_t;

/* On the heap, allocated when a radio first starts: the rings alone are a
   hundred kilobytes, and the kernel image - its zeroed data included - has
   to end below the eight megabytes the firmware leaves free. A machine with
   no radio never pays for it. */
static manager_t *manager_state;
static int32_t state_before_radio;
#define manager (*manager_state)

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

static int same_name(const char *a, uint32_t a_length, const char *b, uint32_t b_length) {
    if (a_length != b_length) {
        return 0;
    }
    for (uint32_t i = 0; i < a_length; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void log_name(const char *ssid, uint32_t length) {
    char text[WIRELESS_SSID_MAX + 1];
    uint32_t n = length > WIRELESS_SSID_MAX ? WIRELESS_SSID_MAX : length;
    for (uint32_t i = 0; i < n; i++) {
        char c = ssid[i];
        text[i] = (c >= 32 && c < 127) ? c : '?';
    }
    text[n] = 0;
    kernel_log_puts(text);
}

static void set_state(int32_t state, int32_t error) {
    uint64_t flags = spin_lock_irqsave(&state_lock);
    manager.state = state;
    manager.error = error;
    spin_unlock_irqrestore(&state_lock, flags);
}

static const char *error_name(int32_t error) {
    switch (error) {
        case WIRELESS_ERROR_NOT_FOUND:
            return "the network is not in range";
        case WIRELESS_ERROR_REJECTED:
            return "the network refused this station";
        case WIRELESS_ERROR_WRONG_PASSWORD:
            return "the handshake failed - the password is wrong";
        case WIRELESS_ERROR_TIMED_OUT:
            return "the network stopped answering";
        case WIRELESS_ERROR_NO_ADDRESS:
            return "no DHCP answer";
        case WIRELESS_ERROR_UNSUPPORTED_SECURITY:
            return "its security is not one this machine speaks";
        case WIRELESS_ERROR_DEVICE:
            return "the radio stopped working";
        case WIRELESS_ERROR_BAD_PASSWORD_FORMAT:
            return "a password is 8 to 63 characters or 64 hexadecimal digits";
        case WIRELESS_ERROR_DISCONNECTED:
            return "the network disconnected this station";
        default:
            return "unknown";
    }
}

static void fail(int32_t error) {
    kernel_log_puts("[wifi] not joined: ");
    kernel_log_puts(error_name(error));
    kernel_log_puts("\n");
    set_state(WIRELESS_STATE_FAILED, error);
}

/* What this machine can join: an open network, or WPA2-Personal with AES on
   both keys. WPA3-only networks need SAE, enterprise networks need 802.1X,
   TKIP and WEP are broken ciphers - each is refused before anything is sent
   rather than failing halfway. A network that offers WPA2 beside WPA3 is
   joined as WPA2, which is what the transition mode exists for. */
static int supported(const ieee80211_network_t *network) {
    if (network->security == IEEE80211_SECURITY_OPEN) {
        return 1;
    }
    if (!(network->security & IEEE80211_SECURITY_WPA2_PSK)) {
        return 0;
    }
    if (!(network->pairwise_ciphers & IEEE80211_CIPHER_CCMP) || network->group_cipher != IEEE80211_CIPHER_CCMP) {
        return 0;
    }
    return (network->rsn_capabilities & IEEE80211_RSN_CAPABILITY_MFP_REQUIRED) == 0;
}

static int secured(const ieee80211_network_t *network) {
    return network->security != IEEE80211_SECURITY_OPEN;
}

static const known_network_t *find_known(const char *ssid, uint32_t length) {
    for (uint32_t i = 0; i < manager.known_count; i++) {
        if (same_name(manager.known[i].ssid, manager.known[i].ssid_length, ssid, length)) {
            return &manager.known[i];
        }
    }
    return 0;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static uint32_t read_hex(const char *text, uint32_t length, uint8_t *out, uint32_t capacity) {
    if (length % 2 || length / 2 > capacity) {
        return 0xFFFFFFFFu;
    }
    for (uint32_t i = 0; i < length / 2; i++) {
        int high = hex_value(text[2 * i]);
        int low = hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return 0xFFFFFFFFu;
        }
        out[i] = (uint8_t)(high << 4 | low);
    }
    return length / 2;
}

static uint32_t write_hex(const uint8_t *bytes, uint32_t length, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < length; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    return length * 2;
}

#define KNOWN_FILE_MAX (WIRELESS_KNOWN_MAX * (2 * WIRELESS_SSID_MAX + 1 + 64 + 1))

static void load_known(void) {
    manager.known_loaded = 1;
    manager.known_count = 0;
    static char text[KNOWN_FILE_MAX + 1];
    int length = wireless_storage_read(text, KNOWN_FILE_MAX);
    if (length <= 0) {
        kernel_log_puts("[wifi] no remembered networks in " WIRELESS_KNOWN_PATH "\n");
        return;
    }
    uint32_t at = 0;
    while (at < (uint32_t)length && manager.known_count < WIRELESS_KNOWN_MAX) {
        uint32_t end = at;
        while (end < (uint32_t)length && text[end] != '\n') {
            end++;
        }
        uint32_t space = at;
        while (space < end && text[space] != ' ') {
            space++;
        }
        known_network_t *entry = &manager.known[manager.known_count];
        uint32_t name = read_hex(text + at, space - at, (uint8_t *)entry->ssid, WIRELESS_SSID_MAX);
        uint32_t key = space < end ? read_hex(text + space + 1, end - space - 1, entry->pmk, 32) : 0;
        if (name != 0xFFFFFFFFu && name > 0 && key == 32) {
            entry->ssid[name] = 0;
            entry->ssid_length = (uint8_t)name;
            manager.known_count++;
        }
        at = end + 1;
    }
    wipe(text, sizeof(text));
    kernel_log_puts("[wifi] ");
    kernel_log_put_dec(manager.known_count);
    kernel_log_puts(" remembered network(s)\n");
}

static int save_known(void) {
    static char text[KNOWN_FILE_MAX];
    uint32_t at = 0;
    for (uint32_t i = 0; i < manager.known_count; i++) {
        at += write_hex((const uint8_t *)manager.known[i].ssid, manager.known[i].ssid_length, text + at);
        text[at++] = ' ';
        at += write_hex(manager.known[i].pmk, 32, text + at);
        text[at++] = '\n';
    }
    int written = wireless_storage_write(text, at) == 0;
    if (!written) {
        kernel_log_puts("[wifi] could not write " WIRELESS_KNOWN_PATH "\n");
    }
    wipe(text, sizeof(text));
    return written;
}

static void remember(const char *ssid, uint32_t length, const uint8_t pmk[32]) {
    known_network_t *entry = (known_network_t *)find_known(ssid, length);
    if (!entry) {
        if (manager.known_count == WIRELESS_KNOWN_MAX) {
            for (uint32_t i = 1; i < WIRELESS_KNOWN_MAX; i++) {
                copy(&manager.known[i - 1], &manager.known[i], sizeof(known_network_t));
            }
            manager.known_count--;
        }
        entry = &manager.known[manager.known_count++];
        copy(entry->ssid, ssid, length);
        entry->ssid[length] = 0;
        entry->ssid_length = (uint8_t)length;
    }
    copy(entry->pmk, pmk, 32);
    if (save_known()) {
        kernel_log_puts("[wifi] remembered ");
        log_name(ssid, length);
        kernel_log_puts(" in " WIRELESS_KNOWN_PATH "\n");
    }
}

static void forget(const char *ssid, uint32_t length) {
    for (uint32_t i = 0; i < manager.known_count; i++) {
        if (same_name(manager.known[i].ssid, manager.known[i].ssid_length, ssid, length)) {
            for (uint32_t j = i + 1; j < manager.known_count; j++) {
                copy(&manager.known[j - 1], &manager.known[j], sizeof(known_network_t));
            }
            manager.known_count--;
            wipe(&manager.known[manager.known_count], sizeof(known_network_t));
            save_known();
            return;
        }
    }
}

/* One row per name, the strongest access point's - a home router on two
   bands, or a house with three of them, is one network to the person
   choosing. A hidden network has no name to choose. */
static void rebuild_rows(void) {
    static os_wireless_network_t rows[WIRELESS_NETWORKS_MAX];
    uint32_t count = 0;
    for (uint32_t i = 0; i < manager.result_count; i++) {
        const ieee80211_network_t *n = &manager.results[i];
        if (n->hidden || n->ssid_length == 0) {
            continue;
        }
        os_wireless_network_t *row = 0;
        for (uint32_t r = 0; r < count; r++) {
            if (same_name(rows[r].ssid, rows[r].ssid_length, n->ssid, n->ssid_length)) {
                row = &rows[r];
            }
        }
        if (row && row->signal_dbm >= manager.result_signal[i]) {
            continue;
        }
        if (!row) {
            if (count == WIRELESS_NETWORKS_MAX) {
                continue;
            }
            row = &rows[count++];
        }
        wipe(row, sizeof(*row));
        copy(row->ssid, n->ssid, n->ssid_length);
        row->ssid_length = n->ssid_length;
        copy(row->bssid, n->bssid, 6);
        row->signal_dbm = manager.result_signal[i];
        row->channel = n->channel;
        row->security = n->security;
        row->supported = (uint8_t)supported(n);
    }
    for (uint32_t i = 1; i < count; i++) {
        os_wireless_network_t held;
        copy(&held, &rows[i], sizeof(held));
        uint32_t j = i;
        while (j > 0 && rows[j - 1].signal_dbm < held.signal_dbm) {
            copy(&rows[j], &rows[j - 1], sizeof(rows[j]));
            j--;
        }
        copy(&rows[j], &held, sizeof(held));
    }
    uint64_t flags = spin_lock_irqsave(&state_lock);
    for (uint32_t i = 0; i < count; i++) {
        rows[i].known = find_known(rows[i].ssid, rows[i].ssid_length) != 0;
        rows[i].connected = manager.phase == PHASE_CONNECTED &&
                            same_name(rows[i].ssid, rows[i].ssid_length, manager.ssid, manager.ssid_length);
        copy(&manager.rows[i], &rows[i], sizeof(rows[i]));
    }
    manager.row_count = count;
    manager.scan_generation++;
    spin_unlock_irqrestore(&state_lock, flags);
}

void wireless_heard(const ieee80211_network_t *network, int8_t signal_dbm) {
    for (uint32_t i = 0; i < manager.heard_count; i++) {
        int same = 1;
        for (int b = 0; b < 6; b++) {
            if (manager.heard[i].bssid[b] != network->bssid[b]) {
                same = 0;
            }
        }
        if (same) {
            if (signal_dbm > manager.heard_signal[i]) {
                manager.heard_signal[i] = signal_dbm;
            }
            return;
        }
    }
    if (manager.heard_count < RESULTS_MAX) {
        copy(&manager.heard[manager.heard_count], network, sizeof(*network));
        manager.heard_signal[manager.heard_count] = signal_dbm;
        manager.heard_count++;
    }
}

static void scan(uint64_t now) {
    if (manager.phase == PHASE_IDLE) {
        set_state(WIRELESS_STATE_SCANNING, manager.error);
    }
    manager.heard_count = 0;
    int result = manager.backend->scan();
    manager.last_scan_ms = now;
    if (result == 0) {
        for (uint32_t i = 0; i < manager.heard_count; i++) {
            copy(&manager.results[i], &manager.heard[i], sizeof(manager.results[i]));
            manager.result_signal[i] = manager.heard_signal[i];
        }
        manager.result_count = manager.heard_count;
    }
    rebuild_rows();
    if (manager.phase == PHASE_IDLE) {
        uint64_t flags = spin_lock_irqsave(&state_lock);
        if (manager.state == WIRELESS_STATE_SCANNING) {
            manager.state = WIRELESS_STATE_IDLE;
        }
        spin_unlock_irqrestore(&state_lock, flags);
    }
}

static int find_network(const char *ssid, uint32_t length, ieee80211_network_t *out, int8_t *signal) {
    int found = -1;
    for (uint32_t i = 0; i < manager.result_count; i++) {
        const ieee80211_network_t *n = &manager.results[i];
        if (!same_name(n->ssid, n->ssid_length, ssid, length)) {
            continue;
        }
        if (found < 0 || manager.result_signal[i] > manager.result_signal[found] ||
            (supported(n) && !supported(&manager.results[found]))) {
            found = (int)i;
        }
    }
    if (found < 0) {
        return 0;
    }
    copy(out, &manager.results[found], sizeof(*out));
    *signal = manager.result_signal[found];
    return 1;
}

static void transmit_eapol(const uint8_t *body, uint32_t length) {
    static uint8_t frame[ETHERNET_HEADER + 512];
    if (length > sizeof(frame) - ETHERNET_HEADER) {
        return;
    }
    copy(frame, manager.network.bssid, 6);
    copy(frame + 6, manager.backend->address, 6);
    frame[12] = (uint8_t)(EAPOL_ETHERTYPE >> 8);
    frame[13] = (uint8_t)EAPOL_ETHERTYPE;
    copy(frame + ETHERNET_HEADER, body, length);
    manager.backend->transmit(frame, ETHERNET_HEADER + length);
}

static void clear_rings(void) {
    uint64_t flags = spin_lock_irqsave(&transmit_lock);
    manager.transmit_head = manager.transmit_tail = 0;
    spin_unlock_irqrestore(&transmit_lock, flags);
    manager.receive_head = manager.receive_tail = 0;
}

static void leave(void) {
    if (manager.phase == PHASE_ADDRESSING || manager.phase == PHASE_CONNECTED) {
        wireless_addressing_stop();
    }
    manager.backend->leave();
    manager.phase = PHASE_IDLE;
    wipe(&manager.handshake, sizeof(manager.handshake));
    wipe(manager.pending_pmk, sizeof(manager.pending_pmk));
    clear_rings();
    uint64_t flags = spin_lock_irqsave(&state_lock);
    manager.ip = 0;
    for (uint32_t i = 0; i < manager.row_count; i++) {
        manager.rows[i].connected = 0;
    }
    spin_unlock_irqrestore(&state_lock, flags);
}

static void begin_addressing(uint64_t now) {
    manager.phase = PHASE_ADDRESSING;
    manager.deadline_ms = now + ADDRESSING_TIMEOUT_MS;
    set_state(WIRELESS_STATE_ADDRESSING, WIRELESS_ERROR_NONE);
    if (manager.remember_after_join) {
        remember(manager.network.ssid, manager.network.ssid_length, manager.pending_pmk);
        rebuild_rows();
    }
    wipe(manager.pending_pmk, sizeof(manager.pending_pmk));
    if (!wireless_addressing_start(manager.backend->address)) {
        kernel_log_puts("[wifi] another link already carries IP - joined without an address of its own\n");
        manager.phase = PHASE_CONNECTED;
        set_state(WIRELESS_STATE_CONNECTED, WIRELESS_ERROR_NONE);
        rebuild_rows();
    }
}

/* The whole join, as far as it can go without waiting on the network: pick
   the access point, turn the password into the key, authenticate and
   associate. The handshake and the address arrive later, as frames. */
static void join_network(const os_wireless_connect_t *request, int from_person, uint64_t now) {
    if (manager.phase != PHASE_IDLE) {
        leave();
    }
    uint32_t length = request->ssid_length > WIRELESS_SSID_MAX ? WIRELESS_SSID_MAX : request->ssid_length;
    uint64_t flags = spin_lock_irqsave(&state_lock);
    copy(manager.ssid, request->ssid, length);
    manager.ssid[length] = 0;
    manager.ssid_length = (uint8_t)length;
    manager.state = WIRELESS_STATE_JOINING;
    manager.error = WIRELESS_ERROR_NONE;
    spin_unlock_irqrestore(&state_lock, flags);

    ieee80211_network_t network;
    int8_t signal = 0;
    if (!find_network(request->ssid, length, &network, &signal)) {
        scan(now);
        set_state(WIRELESS_STATE_JOINING, WIRELESS_ERROR_NONE);
        if (!find_network(request->ssid, length, &network, &signal)) {
            fail(WIRELESS_ERROR_NOT_FOUND);
            return;
        }
    }
    if (!supported(&network)) {
        fail(WIRELESS_ERROR_UNSUPPORTED_SECURITY);
        return;
    }
    copy(&manager.network, &network, sizeof(network));
    manager.secured = secured(&network);
    manager.remember_after_join = 0;
    manager.sent_message_two = 0;
    manager.departed = 0;
    flags = spin_lock_irqsave(&state_lock);
    manager.signal = signal;
    manager.channel = network.channel;
    spin_unlock_irqrestore(&state_lock, flags);

    if (manager.secured) {
        const known_network_t *known = find_known(network.ssid, network.ssid_length);
        if (request->password[0]) {
            if (!wpa_passphrase_to_pmk(request->password, (const uint8_t *)network.ssid, network.ssid_length,
                                       manager.pending_pmk)) {
                fail(WIRELESS_ERROR_BAD_PASSWORD_FORMAT);
                return;
            }
            manager.remember_after_join = from_person && request->remember;
        } else if (known) {
            copy(manager.pending_pmk, known->pmk, 32);
        } else {
            fail(WIRELESS_ERROR_BAD_PASSWORD_FORMAT);
            return;
        }
    } else {
        manager.remember_after_join = from_person && request->remember;
        wipe(manager.pending_pmk, sizeof(manager.pending_pmk));
    }

    kernel_log_puts("[wifi] joining ");
    log_name(network.ssid, network.ssid_length);
    kernel_log_puts(" on channel ");
    kernel_log_put_dec(network.channel);
    kernel_log_puts(manager.secured ? " (WPA2)\n" : " (open)\n");

    uint8_t rsn[64];
    uint32_t rsn_length = manager.secured ? wpa_own_rsn_element(rsn, sizeof(rsn)) : 0;
    if (manager.secured) {
        uint8_t snonce[WPA_NONCE_LENGTH];
        wireless_random(snonce, sizeof(snonce));
        wpa_handshake_begin(&manager.handshake, manager.pending_pmk, manager.backend->address, network.bssid,
                            network.rsn_element, network.rsn_element_length, snonce);
        wipe(snonce, sizeof(snonce));
    }
    manager.phase = manager.secured ? PHASE_SECURING : PHASE_ADDRESSING;
    uint16_t status = 0;
    int joined = manager.backend->join(&network, rsn, rsn_length, &status);
    if (joined != 0) {
        manager.phase = PHASE_IDLE;
        manager.backend->leave();
        kernel_log_puts("[wifi] the network answered status ");
        kernel_log_put_dec(status);
        kernel_log_puts("\n");
        fail(status == 0xFFFF ? WIRELESS_ERROR_TIMED_OUT : WIRELESS_ERROR_REJECTED);
        wipe(manager.pending_pmk, sizeof(manager.pending_pmk));
        return;
    }
    kernel_log_puts("[wifi] associated\n");
    if (manager.secured) {
        manager.deadline_ms = now + SECURING_TIMEOUT_MS;
        set_state(WIRELESS_STATE_SECURING, WIRELESS_ERROR_NONE);
    } else {
        begin_addressing(now);
    }
}

static void handle_eapol(const uint8_t *body, uint32_t length, uint64_t now) {
    if (manager.phase != PHASE_SECURING && manager.phase != PHASE_ADDRESSING && manager.phase != PHASE_CONNECTED) {
        return;
    }
    if (!manager.secured) {
        return;
    }
    static uint8_t reply[512];
    uint32_t reply_length = 0;
    int result = wpa_handshake_receive(&manager.handshake, body, length, reply, sizeof(reply), &reply_length);
    if (result & WPA_RESULT_BAD_MIC) {
        kernel_log_puts("[wifi] a handshake frame with a bad MIC - ignored\n");
        return;
    }
    if (result & WPA_RESULT_REFUSED) {
        kernel_log_puts("[wifi] the handshake asked for something this station does not speak\n");
        leave();
        fail(WIRELESS_ERROR_UNSUPPORTED_SECURITY);
        return;
    }
    if (result & WPA_RESULT_REPLY) {
        transmit_eapol(reply, reply_length);
        if (manager.handshake.state == WPA_HANDSHAKE_SENT_2_OF_4) {
            manager.sent_message_two = 1;
        }
    }
    wipe(reply, sizeof(reply));
    if (result & WPA_RESULT_INSTALL_PAIRWISE) {
        manager.backend->install_key(0, manager.handshake.tk, WPA_KEY_LENGTH, 0);
    }
    if (result & WPA_RESULT_INSTALL_GROUP) {
        manager.backend->install_key(1, manager.handshake.gtk, manager.handshake.gtk_length,
                                     manager.handshake.gtk_index);
        if (!(result & WPA_RESULT_COMPLETE)) {
            kernel_log_puts("[wifi] the network changed its group key\n");
        }
    }
    if ((result & WPA_RESULT_COMPLETE) && manager.phase == PHASE_SECURING) {
        kernel_log_puts("[wifi] the handshake is complete - traffic is encrypted\n");
        begin_addressing(now);
    }
}

static void deliver(const uint8_t *frame, uint32_t length, uint64_t now) {
    if (length < ETHERNET_HEADER) {
        return;
    }
    uint16_t type = (uint16_t)(frame[12] << 8 | frame[13]);
    if (type == EAPOL_ETHERTYPE) {
        handle_eapol(frame + ETHERNET_HEADER, length - ETHERNET_HEADER, now);
    } else if (manager.phase == PHASE_ADDRESSING || manager.phase == PHASE_CONNECTED) {
        wireless_deliver_ip(frame, length);
    }
}

void wireless_received(const uint8_t *ethernet, uint32_t length) {
    if (length > FRAME_MAX || manager.receive_head - manager.receive_tail >= RING_SLOTS) {
        return;
    }
    frame_t *slot = &manager.receive[manager.receive_head % RING_SLOTS];
    copy(slot->bytes, ethernet, length);
    slot->length = length;
    manager.receive_head++;
}

void wireless_departed(uint16_t reason) {
    manager.departed = 1;
    manager.departure_reason = reason;
}

/* The IP stack's way out, from any processor and with the stack's lock held:
   a copy into the ring, which the radio's task empties. */
int wireless_link_send(const uint8_t *frame, uint16_t length) {
    if (!manager_state || length > FRAME_MAX) {
        return -1;
    }
    uint64_t flags = spin_lock_irqsave(&transmit_lock);
    if (manager.transmit_head - manager.transmit_tail >= RING_SLOTS) {
        manager.transmit_dropped++;
        spin_unlock_irqrestore(&transmit_lock, flags);
        return -1;
    }
    frame_t *slot = &manager.transmit[manager.transmit_head % RING_SLOTS];
    copy(slot->bytes, frame, length);
    slot->length = length;
    manager.transmit_head++;
    spin_unlock_irqrestore(&transmit_lock, flags);
    return 0;
}

static void drain_transmit(void) {
    static frame_t frame;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&transmit_lock);
        if (manager.transmit_tail == manager.transmit_head) {
            spin_unlock_irqrestore(&transmit_lock, flags);
            return;
        }
        frame_t *slot = &manager.transmit[manager.transmit_tail % RING_SLOTS];
        copy(frame.bytes, slot->bytes, slot->length);
        frame.length = slot->length;
        manager.transmit_tail++;
        spin_unlock_irqrestore(&transmit_lock, flags);
        if (manager.phase == PHASE_ADDRESSING || manager.phase == PHASE_CONNECTED) {
            manager.backend->transmit(frame.bytes, frame.length);
        }
    }
}

static void take_requests(uint64_t now) {
    uint64_t flags = spin_lock_irqsave(&state_lock);
    int scan_now = manager.want_scan;
    int disconnect_now = manager.want_disconnect;
    int connect_now = manager.want_connect;
    int forget_now = manager.want_forget;
    static os_wireless_connect_t request;
    static char forget_ssid[WIRELESS_SSID_MAX + 1];
    uint8_t forget_length = manager.forget_length;
    if (connect_now) {
        copy(&request, &manager.connect_request, sizeof(request));
        wipe(&manager.connect_request, sizeof(manager.connect_request));
    }
    if (forget_now) {
        copy(forget_ssid, manager.forget_ssid, forget_length);
    }
    manager.want_scan = manager.want_disconnect = manager.want_connect = manager.want_forget = 0;
    spin_unlock_irqrestore(&state_lock, flags);

    if (forget_now) {
        forget(forget_ssid, forget_length);
        if (manager.phase != PHASE_IDLE && same_name(manager.ssid, manager.ssid_length, forget_ssid, forget_length)) {
            leave();
            manager.autojoin_enabled = 0;
            set_state(WIRELESS_STATE_IDLE, WIRELESS_ERROR_NONE);
        }
        rebuild_rows();
    }
    if (disconnect_now) {
        if (manager.phase != PHASE_IDLE) {
            kernel_log_puts("[wifi] leaving the network, as asked\n");
            leave();
        }
        manager.autojoin_enabled = 0;
        set_state(WIRELESS_STATE_IDLE, WIRELESS_ERROR_NONE);
        rebuild_rows();
    }
    if (connect_now) {
        manager.autojoin_enabled = 1;
        join_network(&request, 1, now);
        wipe(&request, sizeof(request));
    }
    if (scan_now) {
        scan(now);
    }
}

static void check_timers(uint64_t now) {
    if (manager.departed) {
        manager.departed = 0;
        kernel_log_puts("[wifi] the network sent this station away, reason ");
        kernel_log_put_dec(manager.departure_reason);
        kernel_log_puts("\n");
        if (manager.phase == PHASE_SECURING) {
            int wrong = manager.sent_message_two;
            leave();
            fail(wrong ? WIRELESS_ERROR_WRONG_PASSWORD : WIRELESS_ERROR_REJECTED);
            manager.next_autojoin_ms = now + AUTOJOIN_RETRY_MS;
        } else if (manager.phase != PHASE_IDLE) {
            leave();
            fail(WIRELESS_ERROR_DISCONNECTED);
            manager.next_autojoin_ms = now + REJOIN_AFTER_DEPARTURE_MS;
        }
    }
    if (manager.phase == PHASE_SECURING && now >= manager.deadline_ms) {
        int wrong = manager.sent_message_two;
        leave();
        fail(wrong ? WIRELESS_ERROR_WRONG_PASSWORD : WIRELESS_ERROR_TIMED_OUT);
        manager.next_autojoin_ms = now + AUTOJOIN_RETRY_MS;
    }
    if (manager.phase == PHASE_ADDRESSING) {
        uint32_t ip = 0;
        int done = wireless_addressing_poll(&ip);
        if (done > 0) {
            manager.phase = PHASE_CONNECTED;
            uint64_t flags = spin_lock_irqsave(&state_lock);
            manager.ip = ip;
            spin_unlock_irqrestore(&state_lock, flags);
            set_state(WIRELESS_STATE_CONNECTED, WIRELESS_ERROR_NONE);
            rebuild_rows();
            kernel_log_puts("[wifi] connected to ");
            log_name(manager.network.ssid, manager.network.ssid_length);
            kernel_log_puts("\n");
        } else if (done < 0 || now >= manager.deadline_ms) {
            leave();
            fail(WIRELESS_ERROR_NO_ADDRESS);
            manager.next_autojoin_ms = now + AUTOJOIN_RETRY_MS;
        }
    }
}

/* With nothing joined and something remembered: look now and then, and join
   the strongest remembered network heard - which is what makes the second
   time a network is used no time at all, and the machine find its network
   by itself after a reboot. A person who chose to disconnect is not
   overridden until they choose a network again. */
static void autojoin(uint64_t now) {
    if (manager.phase != PHASE_IDLE || !manager.autojoin_enabled || manager.known_count == 0 ||
        now < manager.next_autojoin_ms) {
        return;
    }
    if (now - manager.last_scan_ms >= AUTOSCAN_MS) {
        scan(now);
    }
    int best = -1;
    for (uint32_t i = 0; i < manager.result_count; i++) {
        const ieee80211_network_t *n = &manager.results[i];
        if (!n->hidden && supported(n) && find_known(n->ssid, n->ssid_length) &&
            (best < 0 || manager.result_signal[i] > manager.result_signal[best])) {
            best = (int)i;
        }
    }
    manager.next_autojoin_ms = now + AUTOJOIN_RETRY_MS;
    if (best < 0) {
        return;
    }
    static os_wireless_connect_t request;
    wipe(&request, sizeof(request));
    copy(request.ssid, manager.results[best].ssid, manager.results[best].ssid_length);
    request.ssid_length = manager.results[best].ssid_length;
    kernel_log_puts("[wifi] rejoining a remembered network\n");
    join_network(&request, 0, now);
}

void wireless_manager_reset(const wireless_backend_t *backend) {
    if (!manager_state) {
        manager_t *fresh = (manager_t *)wireless_allocate(sizeof(manager_t));
        if (!fresh) {
            state_before_radio = WIRELESS_STATE_FAILED;
            return;
        }
        wipe(fresh, sizeof(*fresh));
        uint64_t flags = spin_lock_irqsave(&state_lock);
        manager_state = fresh;
        spin_unlock_irqrestore(&state_lock, flags);
    }
    uint64_t flags = spin_lock_irqsave(&state_lock);
    wipe(&manager, sizeof(manager));
    spin_unlock_irqrestore(&state_lock, flags);
    manager.backend = backend;
    manager.state = WIRELESS_STATE_IDLE;
    manager.autojoin_enabled = 1;
    manager.scan_generation = 1;
}

void wireless_step(uint64_t now) {
    if (!manager_state) {
        return;
    }
    if (!manager.known_loaded) {
        load_known();
        scan(now);
    }
    if (manager.backend->service() < 0) {
        if (manager.phase != PHASE_IDLE) {
            wireless_addressing_stop();
            manager.phase = PHASE_IDLE;
        }
        set_state(WIRELESS_STATE_FAILED, WIRELESS_ERROR_DEVICE);
        return;
    }
    take_requests(now);
    while (manager.receive_tail != manager.receive_head) {
        frame_t *slot = &manager.receive[manager.receive_tail % RING_SLOTS];
        manager.receive_tail++;
        deliver(slot->bytes, slot->length, now);
    }
    drain_transmit();
    check_timers(now);
    autojoin(now);
}

int wireless_manager_failed(void) {
    return !manager_state || (manager.state == WIRELESS_STATE_FAILED && manager.error == WIRELESS_ERROR_DEVICE);
}

int wireless_manager_busy(void) {
    return manager_state && manager.phase != PHASE_IDLE;
}

void wireless_set_starting(void) {
    if (!manager_state) {
        state_before_radio = WIRELESS_STATE_STARTING;
        return;
    }
    set_state(WIRELESS_STATE_STARTING, WIRELESS_ERROR_NONE);
}

void wireless_set_failed(void) {
    if (!manager_state) {
        state_before_radio = WIRELESS_STATE_FAILED;
        return;
    }
    set_state(WIRELESS_STATE_FAILED, WIRELESS_ERROR_DEVICE);
}

long wireless_status(os_wireless_status_t *out) {
    wipe(out, sizeof(*out));
    uint64_t flags = spin_lock_irqsave(&state_lock);
    if (!manager_state) {
        out->state = state_before_radio;
        out->error = state_before_radio == WIRELESS_STATE_FAILED ? WIRELESS_ERROR_DEVICE : WIRELESS_ERROR_NONE;
        spin_unlock_irqrestore(&state_lock, flags);
        return 0;
    }
    out->state = manager.state;
    out->error = manager.error;
    copy(out->ssid, manager.ssid, manager.ssid_length);
    out->ssid_length = manager.ssid_length;
    if (manager.backend) {
        copy(out->address, manager.backend->address, 6);
    }
    out->ip = manager.ip;
    out->scan_generation = manager.scan_generation;
    out->signal_dbm = manager.signal;
    out->channel = manager.channel;
    spin_unlock_irqrestore(&state_lock, flags);
    return 0;
}

long wireless_networks(os_wireless_network_t *out, uint32_t capacity) {
    uint64_t flags = spin_lock_irqsave(&state_lock);
    if (!manager_state) {
        spin_unlock_irqrestore(&state_lock, flags);
        return 0;
    }
    uint32_t count = manager.row_count < capacity ? manager.row_count : capacity;
    for (uint32_t i = 0; i < count; i++) {
        copy(&out[i], &manager.rows[i], sizeof(out[i]));
    }
    spin_unlock_irqrestore(&state_lock, flags);
    return (long)count;
}

static long radio_present(void) {
    return manager_state && manager.backend && !(manager.state == WIRELESS_STATE_FAILED && manager.error == WIRELESS_ERROR_DEVICE);
}

long wireless_request_scan(void) {
    if (!radio_present()) {
        return -19;
    }
    uint64_t flags = spin_lock_irqsave(&state_lock);
    manager.want_scan = 1;
    if (manager.state == WIRELESS_STATE_IDLE || manager.state == WIRELESS_STATE_FAILED) {
        manager.state = WIRELESS_STATE_SCANNING;
    }
    spin_unlock_irqrestore(&state_lock, flags);
    return 0;
}

long wireless_request_connect(const os_wireless_connect_t *request) {
    if (!radio_present()) {
        return -19;
    }
    if (request->ssid_length == 0 || request->ssid_length > WIRELESS_SSID_MAX) {
        return -22;
    }
    uint64_t flags = spin_lock_irqsave(&state_lock);
    copy(&manager.connect_request, request, sizeof(*request));
    manager.connect_request.password[WIRELESS_PASSWORD_MAX] = 0;
    manager.want_connect = 1;
    copy(manager.ssid, request->ssid, request->ssid_length);
    manager.ssid[request->ssid_length] = 0;
    manager.ssid_length = request->ssid_length;
    manager.state = WIRELESS_STATE_JOINING;
    manager.error = WIRELESS_ERROR_NONE;
    spin_unlock_irqrestore(&state_lock, flags);
    return 0;
}

long wireless_request_disconnect(void) {
    if (!radio_present()) {
        return -19;
    }
    uint64_t flags = spin_lock_irqsave(&state_lock);
    manager.want_disconnect = 1;
    manager.want_connect = 0;
    wipe(&manager.connect_request, sizeof(manager.connect_request));
    spin_unlock_irqrestore(&state_lock, flags);
    return 0;
}

long wireless_request_forget(const char *ssid, uint32_t ssid_length) {
    if (!radio_present()) {
        return -19;
    }
    if (ssid_length == 0 || ssid_length > WIRELESS_SSID_MAX) {
        return -22;
    }
    uint64_t flags = spin_lock_irqsave(&state_lock);
    copy(manager.forget_ssid, ssid, ssid_length);
    manager.forget_length = (uint8_t)ssid_length;
    manager.want_forget = 1;
    spin_unlock_irqrestore(&state_lock, flags);
    return 0;
}
