#include "intel_wireless.h"

#include "drivers/intel_wireless_api.h"
#include "drivers/intel_wireless_firmware.h"
#include "drivers/intel_wireless_registers.h"
#include "drivers/intel_wireless_transport.h"
#include "drivers/kernel_log.h"
#include "memory_management/heap.h"
#include "network/wireless_manager.h"
#ifndef LEANOS_HOST_TEST
#include "architecture/x86_64/io.h"
#include "drivers/pci.h"
#include "file_system/virtual_file_system.h"
#include "memory_management/virtual_memory.h"
#include "scheduler/scheduler.h"
#endif

#define INTEL_VENDOR 0x8086

#define COMMAND_TIMEOUT_MS 2000
#define ALIVE_TIMEOUT_MS 1000
#define INIT_COMPLETE_TIMEOUT_MS 2000
#define SCAN_TIMEOUT_MS 15000
#define SESSION_START_TIMEOUT_MS 300
#define SESSION_DURATION_TU 585
#define AUTHENTICATION_TIMEOUT_MS 300
#define ASSOCIATION_TIMEOUT_MS 500
#define JOIN_ATTEMPTS 3
#define FIRMWARE_RESTARTS_MAX 5
#define STATION_ID 0
#define REASON_LEAVING 3

#define NVM_CHANNEL_VALID 0x1u

#define MAC_ID 0

/* The NVM's channel table, in the order its flags come in - the first 51 of
   Linux's iwl_unii9_nvm_channels, which is the extended NVM layout every
   22000-family device without 6 GHz uses. */
static const uint8_t nvm_channels[] = {
    1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  36,  40,  44,
    48,  52,  56,  60,  64,  68,  72,  76,  80,  84,  88,  92,  96,  100, 104, 108, 112,
    116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177, 181,
};
#define NVM_CHANNEL_COUNT (sizeof(nvm_channels) / sizeof(nvm_channels[0]))

typedef struct {
    int state;
    volatile uint8_t *registers;
    const uint8_t *firmware_file;
    uint32_t firmware_length;
    uint32_t pci_device_id;
    uint32_t hardware_revision;
    uint32_t rf_id;
    intel_wireless_firmware_t firmware;
    intel_wireless_alive_v6_t alive;
    uint8_t mac_address[6];
    int have_mac_address;
    uint8_t tx_antennas;
    uint8_t rx_antennas;
    uint32_t channel_flags[NVM_CHANNEL_COUNT];
    uint32_t channel_flag_count;

    int init_complete;
    int scan_complete;
    intel_wireless_scan_complete_t scan_result;
    uint32_t frames_seen;
    uint32_t scan_generation;

    intel_wireless_network_t networks[INTEL_WIRELESS_MAX_NETWORKS];
    uint32_t network_count;
    volatile int scan_requested;

    /* The network being joined, or joined, and how far along. */
    int joining;
    int associated;
    int phy_added;
    int binding_added;
    int station_added;
    int queue_added;
    int pairwise_key;
    int authorized;
    ieee80211_network_t target;
    volatile int session_started;
    volatile int authentication_answered;
    uint16_t authentication_status;
    volatile int association_answered;
    uint16_t association_status;
    uint16_t association_id;
    int have_sync;
    uint64_t sync_tsf;
    uint32_t sync_device_time;
    uint8_t sync_dtim_count;
    uint8_t sync_dtim_period;
    uint64_t last_unicast_pn;
    uint64_t last_group_pn;
    uint32_t data_frames_received;
    uint32_t frames_dropped;
    uint32_t transmit_failures;
    uint32_t frames_queued_at_join;
} wireless_t;

static wireless_t wireless;

static void log_hex(const char *label, uint32_t value) {
    kernel_log_puts(label);
    kernel_log_put_hex32(value);
}

static void log_result(const char *what, int result) {
    kernel_log_puts("[wifi] ");
    kernel_log_puts(what);
    if (result == INTEL_WIRELESS_OK) {
        kernel_log_puts(": ok\n");
        return;
    }
    kernel_log_puts(": failed, code -");
    kernel_log_put_dec((uint32_t)(-result));
    kernel_log_puts("\n");
}

static void copy_bytes(void *to, const void *from, uint32_t length) {
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

/* What the device says when it stops: the boot ROM's status words, the
   program counters, and - once the firmware has run - the first words of its
   own error tables. The laptop has no serial port; this is what the stick
   log will carry back. */
static void log_diagnostics(void) {
    kernel_log_puts("[wifi] state:");
    log_hex(" GP_CNTRL ", intel_wireless_read32(CSR_GP_CNTRL));
    log_hex(" INT ", intel_wireless_read32(CSR_INT));
    log_hex(" HW_IF ", intel_wireless_read32(CSR_HW_IF_CONFIG_REG));
    log_hex(" RESET ", intel_wireless_read32(CSR_RESET));
    log_hex(" GP1 ", intel_wireless_read32(CSR_UCODE_DRV_GP1));
    log_hex(" SB1 ", intel_wireless_read_periphery(UMAG_SB_CPU_1_STATUS));
    log_hex(" SB2 ", intel_wireless_read_periphery(UMAG_SB_CPU_2_STATUS));
    log_hex(" UMAC_PC ", intel_wireless_read_periphery(0xA05C18u));
    log_hex(" LMAC_PC ", intel_wireless_read_periphery(0xA05C1Cu));
    kernel_log_puts("\n");

    uint32_t tables[2] = {wireless.alive.lmac[0].debug.error_event_table_pointer,
                          wireless.alive.umac.error_info_address & ~0xC0000000u};
    const char *names[2] = {"lmac", "umac"};
    for (int t = 0; t < 2; t++) {
        if (!tables[t]) {
            continue;
        }
        uint32_t words[24];
        if (!intel_wireless_read_device_memory(tables[t], words, 24)) {
            continue;
        }
        kernel_log_puts("[wifi] ");
        kernel_log_puts(names[t]);
        kernel_log_puts(" error table at ");
        kernel_log_put_hex32(tables[t]);
        kernel_log_puts(":");
        for (int i = 0; i < 24; i++) {
            kernel_log_puts(" ");
            kernel_log_put_hex32(words[i]);
        }
        kernel_log_puts("\n");
    }
}

static void fail(const char *what, int result) {
    log_result(what, result);
    log_diagnostics();
    intel_wireless_stop();
    wireless.state = INTEL_WIRELESS_STATE_FAILED;
}

static int8_t signal_of(const intel_wireless_rx_mpdu_v1_t *descriptor) {
    uint8_t a = descriptor->energy_a;
    uint8_t b = descriptor->energy_b;
    uint8_t best = a && b ? (a < b ? a : b) : (a ? a : b);
    return best ? (int8_t)-(int)best : -127;
}

static void remember_network(const ieee80211_network_t *network, const intel_wireless_rx_mpdu_v1_t *descriptor) {
    int8_t signal = signal_of(descriptor);
    uint8_t channel = network->channel ? network->channel : descriptor->channel;
    for (uint32_t i = 0; i < wireless.network_count; i++) {
        intel_wireless_network_t *known = &wireless.networks[i];
        int same = 1;
        for (int b = 0; b < 6; b++) {
            if (known->network.bssid[b] != network->bssid[b]) {
                same = 0;
            }
        }
        if (same) {
            if (signal > known->signal_dbm) {
                known->signal_dbm = signal;
            }
            known->seen++;
            known->network.timestamp = network->timestamp;
            known->network.dtim_count = network->dtim_count;
            known->sync_device_time = descriptor->gp2_on_air_rise;
            return;
        }
    }
    uint32_t slot = wireless.network_count;
    if (slot >= INTEL_WIRELESS_MAX_NETWORKS) {
        uint32_t weakest = 0;
        for (uint32_t i = 1; i < wireless.network_count; i++) {
            if (wireless.networks[i].signal_dbm < wireless.networks[weakest].signal_dbm) {
                weakest = i;
            }
        }
        if (wireless.networks[weakest].signal_dbm >= signal) {
            return;
        }
        slot = weakest;
    } else {
        wireless.network_count++;
    }
    wireless.networks[slot].network = *network;
    wireless.networks[slot].network.channel = channel;
    wireless.networks[slot].signal_dbm = signal;
    wireless.networks[slot].band = channel > 14 ? PHY_BAND_5 : PHY_BAND_24;
    wireless.networks[slot].seen = 1;
    wireless.networks[slot].sync_device_time = descriptor->gp2_on_air_rise;
}

static uint16_t read16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | bytes[1] << 8);
}

static int same_address(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void handle_management(const uint8_t *frame, uint32_t length, const intel_wireless_rx_mpdu_v1_t *descriptor) {
    ieee80211_network_t network;
    if (ieee80211_parse_beacon(frame, length, &network)) {
        if (wireless.state == INTEL_WIRELESS_STATE_SCANNING) {
            remember_network(&network, descriptor);
        }
        if (wireless.joining && same_address(network.bssid, wireless.target.bssid)) {
            wireless.have_sync = 1;
            wireless.sync_tsf = network.timestamp;
            wireless.sync_device_time = descriptor->gp2_on_air_rise;
            wireless.sync_dtim_count = network.dtim_count;
            if (network.dtim_period) {
                wireless.sync_dtim_period = network.dtim_period;
            }
        }
        return;
    }
    if (!wireless.joining && !wireless.associated) {
        return;
    }
    uint16_t sequence = 0;
    uint16_t status = 0;
    uint16_t association_id = 0;
    uint16_t reason = 0;
    if (ieee80211_parse_authentication(frame, length, wireless.mac_address, wireless.target.bssid, &sequence,
                                       &status)) {
        if (sequence == 2) {
            wireless.authentication_status = status;
            wireless.authentication_answered = 1;
        }
    } else if (ieee80211_parse_association_response(frame, length, wireless.mac_address, wireless.target.bssid,
                                                    &status, &association_id)) {
        wireless.association_status = status;
        wireless.association_id = association_id;
        wireless.association_answered = 1;
    } else if (ieee80211_parse_departure(frame, length, wireless.mac_address, wireless.target.bssid, &reason)) {
        if (wireless.associated) {
            wireless_departed(reason);
        }
    }
}

/* A CCMP packet number only goes up. The radio checks the MIC; the replay
   check is the receiver's. */
static int fresh_packet_number(const uint8_t *frame, uint32_t header_length) {
    const uint8_t *ccmp = frame + header_length;
    uint64_t pn = (uint64_t)ccmp[0] | (uint64_t)ccmp[1] << 8 | (uint64_t)ccmp[4] << 16 | (uint64_t)ccmp[5] << 24 |
                  (uint64_t)ccmp[6] << 32 | (uint64_t)ccmp[7] << 40;
    uint64_t *last = (frame[4] & 1) ? &wireless.last_group_pn : &wireless.last_unicast_pn;
    if (pn <= *last) {
        return 0;
    }
    *last = pn;
    return 1;
}

/* A data frame as the firmware hands it over: the 802.11 header, then the
   CCMP header when the frame was protected (decrypted already, its MIC
   checked and removed), then two bytes of padding when the header was not a
   whole number of words, then the payload - and, at the end, whatever of the
   MIC or CRC the hardware left, whose length the descriptor carries. */
/* An access point sends the handshake's first message right behind its
   association response - often in the same receive buffer - so data is
   taken from the moment the response said yes, not from when this side
   finished writing it down. */
static int accepting_data(void) {
    return wireless.associated || (wireless.joining && wireless.association_answered &&
                                   wireless.association_status == IEEE80211_STATUS_SUCCESS);
}

static void handle_data(const uint8_t *frame, uint32_t length, const intel_wireless_rx_mpdu_v1_t *descriptor) {
    if (!accepting_data() || length < IEEE80211_HEADER_LENGTH) {
        return;
    }
    uint32_t pad = (descriptor->mac_flags2 & IWL_RX_MPDU_MFLG2_PAD) ? 2 : 0;
    uint32_t trailer = ((descriptor->mac_flags1 & IWL_RX_MPDU_MFLG1_MIC_CRC_LEN_MASK) >> 4) * 2;
    uint32_t header_length = ieee80211_header_length(frame);
    uint32_t content = length - pad;
    if (content > trailer) {
        content -= trailer;
    }
    uint16_t frame_control = read16(frame);
    uint32_t security = 0;
    if (frame_control & IEEE80211_FRAME_CONTROL_PROTECTED) {
        if ((descriptor->status & IWL_RX_MPDU_STATUS_SEC_MASK) != IWL_RX_MPDU_STATUS_SEC_CCM ||
            !(descriptor->status & IWL_RX_MPDU_STATUS_MIC_OK) || length < header_length + IEEE80211_CCMP_HEADER_LENGTH ||
            !fresh_packet_number(frame, header_length)) {
            wireless.frames_dropped++;
            return;
        }
        security = IEEE80211_CCMP_HEADER_LENGTH;
    }
    static uint8_t ethernet[2048];
    uint32_t ethernet_length = ieee80211_data_to_ethernet(frame, content + pad, header_length + security + pad,
                                                          wireless.target.bssid, ethernet, sizeof(ethernet));
    if (ethernet_length == 0) {
        return;
    }
    uint16_t type = (uint16_t)(ethernet[12] << 8 | ethernet[13]);
    if (!security && wireless.pairwise_key && type != 0x888E) {
        wireless.frames_dropped++;
        return;
    }
    wireless.data_frames_received++;
    wireless_received(ethernet, ethernet_length);
}

static void handle_frame(const uint8_t *payload, uint32_t payload_length) {
    if (payload_length < sizeof(intel_wireless_rx_mpdu_v1_t)) {
        return;
    }
    const intel_wireless_rx_mpdu_v1_t *descriptor = (const intel_wireless_rx_mpdu_v1_t *)payload;
    uint32_t length = descriptor->mpdu_length;
    if (length + sizeof(*descriptor) > payload_length) {
        return;
    }
    if ((descriptor->status & (IWL_RX_MPDU_STATUS_CRC_OK | IWL_RX_MPDU_STATUS_OVERRUN_OK)) !=
        (IWL_RX_MPDU_STATUS_CRC_OK | IWL_RX_MPDU_STATUS_OVERRUN_OK)) {
        return;
    }
    wireless.frames_seen++;
    const uint8_t *frame = payload + sizeof(*descriptor);
    if (length < IEEE80211_HEADER_LENGTH) {
        return;
    }
    uint16_t type = read16(frame) & IEEE80211_FRAME_CONTROL_TYPE_MASK;
    if (type == IEEE80211_TYPE_MANAGEMENT) {
        handle_management(frame, length, descriptor);
    } else if (type == IEEE80211_TYPE_DATA) {
        handle_data(frame, length, descriptor);
    }
}

static void handle_transmit_response(const uint8_t *payload, uint32_t payload_length) {
    if (payload_length < sizeof(intel_wireless_tx_response_t)) {
        return;
    }
    const intel_wireless_tx_response_t *response = (const intel_wireless_tx_response_t *)payload;
    if ((int)response->tx_queue != intel_wireless_data_queue_number()) {
        return;
    }
    intel_wireless_data_completed(response->frame_count ? response->frame_count : 1);
    uint32_t status = response->status & TX_STATUS_MSK;
    if (status != TX_STATUS_SUCCESS && status != TX_STATUS_DIRECT_DONE) {
        if (wireless.transmit_failures++ < 8) {
            kernel_log_puts("[wifi] a frame was not delivered, status ");
            kernel_log_put_hex32(status);
            kernel_log_puts("\n");
        }
    }
}

static void handle_packet(const intel_wireless_rx_packet_t *packet, const uint8_t *payload,
                          uint32_t payload_length) {
    int legacy = packet->group <= LONG_GROUP;
    if (legacy && packet->command == INIT_COMPLETE_NOTIF) {
        wireless.init_complete = 1;
    } else if (legacy && packet->command == REPLY_RX_MPDU_CMD) {
        handle_frame(payload, payload_length);
    } else if (legacy && packet->command == TX_CMD) {
        handle_transmit_response(payload, payload_length);
    } else if (packet->group == MAC_CONF_GROUP && packet->command == SESSION_PROTECTION_NOTIF) {
        if (payload_length >= sizeof(intel_wireless_session_protection_notification_t)) {
            const intel_wireless_session_protection_notification_t *notification =
                (const intel_wireless_session_protection_notification_t *)payload;
            if (notification->start) {
                wireless.session_started = 1;
            }
        }
    } else if (legacy && packet->command == SCAN_COMPLETE_UMAC) {
        if (payload_length >= sizeof(intel_wireless_scan_complete_t)) {
            copy_bytes(&wireless.scan_result, payload, sizeof(wireless.scan_result));
        }
        wireless.scan_complete = 1;
    } else if (legacy && packet->command == REPLY_ERROR && payload_length >= sizeof(intel_wireless_error_response_t)) {
        const intel_wireless_error_response_t *error = (const intel_wireless_error_response_t *)payload;
        kernel_log_puts("[wifi] the firmware refused command ");
        kernel_log_put_hex32(error->command);
        log_hex(": error ", error->error_type);
        log_hex(" info ", error->error_info);
        kernel_log_puts("\n");
    }
}

static int wait_for(volatile int *flag, uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited <= timeout_ms; waited++) {
        int polled = intel_wireless_poll();
        if (polled != INTEL_WIRELESS_OK) {
            return polled;
        }
        if (*flag) {
            return INTEL_WIRELESS_OK;
        }
        intel_wireless_sleep_ms(1);
    }
    return INTEL_WIRELESS_TIMED_OUT;
}

static int send(uint8_t group, uint8_t command, const void *payload, uint32_t length, void *response,
                uint32_t capacity, uint32_t *response_length) {
    uint8_t version = intel_wireless_command_version(&wireless.firmware, group, command, 0);
    if (version == INTEL_WIRELESS_COMMAND_VERSION_UNKNOWN) {
        version = 0;
    }
    return intel_wireless_send(group, command, version, payload, length, response, capacity, response_length,
                               COMMAND_TIMEOUT_MS);
}

static int read_nvm(void) {
    intel_wireless_nvm_get_info_t request = {0};
    static intel_wireless_nvm_get_info_response_v4_t response;
    uint32_t length = 0;
    int result = send(REGULATORY_AND_NVM_GROUP, NVM_GET_INFO, &request, sizeof(request), &response,
                      sizeof(response), &length);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    if (length < sizeof(response) - sizeof(response.channel_profile)) {
        return INTEL_WIRELESS_TOO_LARGE;
    }
    if (response.flags & NVM_GENERAL_FLAGS_EMPTY_OTP) {
        kernel_log_puts("[wifi] the device's NVM is empty - no address, no calibration\n");
    }
    wireless.tx_antennas = wireless.firmware.valid_tx_antennas;
    wireless.rx_antennas = wireless.firmware.valid_rx_antennas;
    if (response.tx_chains) {
        wireless.tx_antennas &= (uint8_t)response.tx_chains;
    }
    if (response.rx_chains) {
        wireless.rx_antennas &= (uint8_t)response.rx_chains;
    }
    uint32_t count = response.channel_count;
    if (count > NVM_CHANNEL_COUNT) {
        count = NVM_CHANNEL_COUNT;
    }
    wireless.channel_flag_count = count;
    for (uint32_t i = 0; i < count; i++) {
        wireless.channel_flags[i] = response.channel_profile[i];
    }

    kernel_log_puts("[wifi] NVM: version ");
    kernel_log_put_hex32(response.nvm_version);
    kernel_log_puts(", bands");
    kernel_log_puts(response.mac_sku_flags & NVM_MAC_SKU_FLAGS_BAND_2_4_ENABLED ? " 2.4" : "");
    kernel_log_puts(response.mac_sku_flags & NVM_MAC_SKU_FLAGS_BAND_5_2_ENABLED ? " 5" : "");
    kernel_log_puts(" GHz, antennas tx ");
    kernel_log_put_hex32(wireless.tx_antennas);
    kernel_log_puts(" rx ");
    kernel_log_put_hex32(wireless.rx_antennas);
    kernel_log_puts(", LAR ");
    kernel_log_puts(response.lar_enabled ? "on" : "off");
    kernel_log_puts(", ");
    kernel_log_put_dec(response.channel_count);
    kernel_log_puts(" channels\n");
    return INTEL_WIRELESS_OK;
}

/* The device's address is not in the NVM answer on this family. It is in two
   register pairs the firmware fills: what the laptop's maker strapped, and
   behind it what Intel fused into the OTP - the first one that is a usable
   address wins, which is Linux's order. The bytes are stored backwards. */
#define CSR_MAC_ADDRESS_OTP   0x380
#define CSR_MAC_ADDRESS_STRAP 0x388

static int address_is_usable(const uint8_t *address) {
    int any = 0;
    for (int i = 0; i < 6; i++) {
        if (address[i] != 0) {
            any = 1;
        }
    }
    return any && (address[0] & 1) == 0;
}

static void address_from(uint32_t base, uint8_t *out) {
    uint32_t low = intel_wireless_read32(base);
    uint32_t high = intel_wireless_read32(base + 4);
    out[0] = (uint8_t)(low >> 24);
    out[1] = (uint8_t)(low >> 16);
    out[2] = (uint8_t)(low >> 8);
    out[3] = (uint8_t)low;
    out[4] = (uint8_t)(high >> 8);
    out[5] = (uint8_t)high;
}

static void read_mac_address(void) {
    address_from(CSR_MAC_ADDRESS_STRAP, wireless.mac_address);
    if (!address_is_usable(wireless.mac_address)) {
        address_from(CSR_MAC_ADDRESS_OTP, wireless.mac_address);
    }
    wireless.have_mac_address = address_is_usable(wireless.mac_address);
    kernel_log_puts("[wifi] address ");
    for (int i = 0; i < 6; i++) {
        static const char digits[] = "0123456789abcdef";
        char text[4] = {digits[wireless.mac_address[i] >> 4], digits[wireless.mac_address[i] & 15], i < 5 ? ':' : 0,
                        0};
        kernel_log_puts(text);
    }
    kernel_log_puts(wireless.have_mac_address ? "\n" : " (not a usable address)\n");
}

static int update_mcc(void) {
    intel_wireless_mcc_update_t request;
    zero(&request, sizeof(request));
    request.mcc = (uint16_t)(('Z' << 8) | 'Z');
    request.source_id = MCC_SOURCE_GET_CURRENT;
    uint8_t response[512];
    uint32_t length = 0;
    int result = send(LEGACY_GROUP, MCC_UPDATE_CMD, &request, sizeof(request), response, sizeof(response), &length);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    if (length >= sizeof(intel_wireless_mcc_update_response_v4_t)) {
        const intel_wireless_mcc_update_response_v4_t *answer = (const intel_wireless_mcc_update_response_v4_t *)response;
        char country[3] = {(char)(answer->mcc >> 8), (char)answer->mcc, 0};
        kernel_log_puts("[wifi] regulatory domain ");
        kernel_log_puts(country);
        log_hex(", status ", answer->status);
        kernel_log_puts(", ");
        kernel_log_put_dec(answer->channel_count);
        kernel_log_puts(" channels\n");
    }
    return INTEL_WIRELESS_OK;
}

static uint32_t soc_latency_flags(uint32_t device, uint32_t *latency) {
    if (device == 0xA0F0 || device == 0x43F0) {
        *latency = 12000;
        return (2u << SOC_FLAGS_LTR_APPLY_DELAY_POSITION) | SOC_CONFIG_CMD_FLAGS_LOW_LATENCY;
    }
    if (device == 0x34F0 || device == 0x3DF0 || device == 0x4DF0) {
        *latency = 1820;
        return 3u << SOC_FLAGS_LTR_APPLY_DELAY_POSITION;
    }
    *latency = 500;
    return 1u << SOC_FLAGS_LTR_APPLY_DELAY_POSITION;
}

/* The station's MAC context: before joining it listens to everything with a
   broadcast BSSID; joined, it carries the network's BSSID and - once
   associated - the beacon timing the firmware wakes for. Each access
   category gets its own transmit FIFO, which on this family is the
   category's number plus one. */
static int mac_context(uint32_t action, const uint8_t *bssid, int associated) {
    static intel_wireless_mac_context_t context;
    zero(&context, sizeof(context));
    context.id_and_color = MAC_ID | (0u << FW_CTXT_COLOR_POSITION);
    context.action = action;
    context.mac_type = FW_MAC_TYPE_BSS_STA;
    context.tsf_id = 0;
    copy_bytes(context.node_address, wireless.mac_address, 6);
    for (int i = 0; i < 6; i++) {
        context.bssid_address[i] = bssid ? bssid[i] : 0xFF;
    }
    int five = bssid && wireless.target.channel > 14;
    context.cck_rates = five ? 0 : 0x0F;
    context.ofdm_rates = 0x15;
    if (five || (bssid && (wireless.target.capability & IEEE80211_CAPABILITY_SHORT_SLOT))) {
        context.short_slot = MAC_FLG_SHORT_SLOT;
    }
    context.filter_flags = MAC_FILTER_ACCEPT_GRP | (associated ? 0 : MAC_FILTER_IN_BEACON);
    static const struct {
        uint16_t cw_min, cw_max;
        uint8_t aifsn;
        uint16_t txop;
    } defaults[INTEL_WIRELESS_AC_COUNT] = {{15, 1023, 7, 0}, {15, 1023, 3, 0}, {7, 15, 2, 94}, {3, 7, 2, 47}};
    for (int ac = 0; ac < INTEL_WIRELESS_AC_COUNT; ac++) {
        context.ac[ac].cw_min = defaults[ac].cw_min;
        context.ac[ac].cw_max = defaults[ac].cw_max;
        context.ac[ac].aifsn = defaults[ac].aifsn;
        context.ac[ac].edca_txop = (uint16_t)(defaults[ac].txop * 32);
        context.ac[ac].fifos_mask = (uint8_t)(1u << (ac + 1));
    }
    context.station.listen_interval = 10;
    if (associated) {
        uint32_t interval = wireless.target.beacon_interval ? wireless.target.beacon_interval : 100;
        uint32_t period = wireless.sync_dtim_period ? wireless.sync_dtim_period : 1;
        uint64_t offset = (uint64_t)wireless.sync_dtim_count * interval * 1024;
        context.station.is_assoc = 1;
        context.station.dtim_tsf = wireless.sync_tsf + offset;
        context.station.dtim_time = (uint32_t)(wireless.sync_device_time + offset);
        context.station.assoc_beacon_arrive_time = wireless.sync_device_time;
        context.station.beacon_interval = interval;
        context.station.dtim_interval = interval * period;
        context.station.assoc_id = wireless.association_id;
    }
    return send(LEGACY_GROUP, MAC_CONTEXT_CMD, &context, sizeof(context), 0, 0, 0);
}

static int add_mac_context(void) {
    return mac_context(FW_CTXT_ACTION_ADD, 0, 0);
}

static int configure(void) {
    intel_wireless_init_extended_config_t init = {1u << IWL_INIT_NVM};
    int result = send(SYSTEM_GROUP, INIT_EXTENDED_CFG_CMD, &init, sizeof(init), 0, 0, 0);
    log_result("init configuration", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    intel_wireless_nvm_access_complete_t complete = {0};
    result = send(REGULATORY_AND_NVM_GROUP, NVM_ACCESS_COMPLETE, &complete, sizeof(complete), 0, 0, 0);
    log_result("NVM access complete", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    result = wait_for(&wireless.init_complete, INIT_COMPLETE_TIMEOUT_MS);
    log_result("init complete", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }

    result = read_nvm();
    log_result("NVM", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    read_mac_address();

    intel_wireless_tx_antenna_config_t antennas = {wireless.tx_antennas};
    result = send(LEGACY_GROUP, TX_ANT_CONFIGURATION_CMD, &antennas, sizeof(antennas), 0, 0, 0);
    log_result("antennas", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }

    intel_wireless_bt_coex_t coex = {BT_COEX_NW, BT_COEX_HIGH_BAND_RET};
    if (intel_wireless_firmware_has_capability(&wireless.firmware, IWL_UCODE_TLV_CAPA_BT_MPLUT_SUPPORT)) {
        coex.enabled_modules |= BT_COEX_MPLUT_ENABLED;
    }
    result = send(LEGACY_GROUP, BT_CONFIG, &coex, sizeof(coex), 0, 0, 0);
    log_result("Bluetooth coexistence", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }

    if (intel_wireless_firmware_has_capability(&wireless.firmware, IWL_UCODE_TLV_CAPA_SOC_LATENCY_SUPPORT)) {
        intel_wireless_soc_configuration_t soc;
        uint32_t latency = 0;
        soc.flags = soc_latency_flags(wireless.pci_device_id, &latency);
        soc.latency = latency;
        result = send(SYSTEM_GROUP, SOC_CONFIGURATION_CMD, &soc, sizeof(soc), 0, 0, 0);
        log_result("crystal latency", result);
        if (result != INTEL_WIRELESS_OK) {
            return result;
        }
    }

    if (intel_wireless_firmware_has_capability(&wireless.firmware, IWL_UCODE_TLV_CAPA_DQA_SUPPORT)) {
        intel_wireless_dqa_enable_t dqa = {INTEL_WIRELESS_COMMAND_QUEUE};
        result = send(DATA_PATH_GROUP, DQA_ENABLE_CMD, &dqa, sizeof(dqa), 0, 0, 0);
        log_result("DQA", result);
        if (result != INTEL_WIRELESS_OK) {
            return result;
        }
    }

    if (intel_wireless_firmware_has_capability(&wireless.firmware, IWL_UCODE_TLV_CAPA_LAR_SUPPORT)) {
        result = update_mcc();
        log_result("regulatory", result);
        if (result != INTEL_WIRELESS_OK) {
            return result;
        }
    }

    intel_wireless_scan_config_t scan_config;
    zero(&scan_config, sizeof(scan_config));
    scan_config.tx_chains = wireless.tx_antennas;
    scan_config.rx_chains = wireless.rx_antennas;
    result = send(LEGACY_GROUP, SCAN_CFG_CMD, &scan_config, sizeof(scan_config), 0, 0, 0);
    log_result("scan configuration", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }

    result = add_mac_context();
    log_result("station context", result);
    return result;
}

static int start_firmware(void) {
    int result = intel_wireless_start_hardware();
    log_result("hardware", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    if (intel_wireless_radio_switch_off()) {
        kernel_log_puts("[wifi] the radio is switched off (rfkill) - starting the firmware anyway\n");
    }
    result = intel_wireless_start_firmware(&wireless.firmware, wireless.hardware_revision);
    log_result("firmware handed over", result);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    zero(&wireless.alive, sizeof(wireless.alive));
    result = intel_wireless_wait_alive(&wireless.alive, ALIVE_TIMEOUT_MS);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    kernel_log_puts("[wifi] alive:");
    log_hex(" status ", wireless.alive.status);
    log_hex(" lmac ", wireless.alive.lmac[0].ucode_major);
    log_hex(".", wireless.alive.lmac[0].ucode_minor);
    log_hex(" umac ", wireless.alive.umac.umac_major);
    log_hex(".", wireless.alive.umac.umac_minor);
    log_hex(" sku ", wireless.alive.sku_id[0]);
    log_hex(" ", wireless.alive.sku_id[1]);
    log_hex(" ", wireless.alive.sku_id[2]);
    kernel_log_puts("\n");
    if (wireless.alive.status != IWL_ALIVE_STATUS_OK) {
        return INTEL_WIRELESS_BAD_ALIVE;
    }
    intel_wireless_firmware_alive();
    return INTEL_WIRELESS_OK;
}

int intel_wireless_bring_up(volatile uint8_t *registers, uint32_t pci_device_id, const uint8_t *firmware_file,
                            uint32_t firmware_length) {
    wireless.state = INTEL_WIRELESS_STATE_STARTING;
    wireless.registers = registers;
    wireless.firmware_file = firmware_file;
    wireless.firmware_length = firmware_length;
    wireless.pci_device_id = pci_device_id;
    wireless.init_complete = 0;
    intel_wireless_transport_attach(registers);
    intel_wireless_set_packet_handler(handle_packet);

    int parsed = intel_wireless_firmware_parse(firmware_file, firmware_length, &wireless.firmware);
    if (parsed != 0) {
        kernel_log_puts("[wifi] the firmware file did not parse, code -");
        kernel_log_put_dec((uint32_t)(-parsed));
        kernel_log_puts("\n");
        wireless.state = INTEL_WIRELESS_STATE_FAILED;
        return INTEL_WIRELESS_FIRMWARE_ERROR;
    }
    kernel_log_puts("[wifi] firmware ");
    kernel_log_puts(wireless.firmware.human_readable);
    kernel_log_puts(": ");
    kernel_log_put_dec(wireless.firmware.lmac_count);
    kernel_log_puts(" LMAC, ");
    kernel_log_put_dec(wireless.firmware.umac_count);
    kernel_log_puts(" UMAC and ");
    kernel_log_put_dec(wireless.firmware.paging_count);
    kernel_log_puts(" paging sections\n");

    int result = start_firmware();
    if (result != INTEL_WIRELESS_OK) {
        fail("firmware start", result);
        return result;
    }
    result = configure();
    if (result != INTEL_WIRELESS_OK) {
        fail("configuration", result);
        return result;
    }
    wireless.state = intel_wireless_radio_switch_off() ? INTEL_WIRELESS_STATE_RADIO_OFF : INTEL_WIRELESS_STATE_READY;
    return INTEL_WIRELESS_OK;
}

static void sort_networks(void) {
    for (uint32_t i = 1; i < wireless.network_count; i++) {
        intel_wireless_network_t held = wireless.networks[i];
        uint32_t j = i;
        while (j > 0 && wireless.networks[j - 1].signal_dbm < held.signal_dbm) {
            wireless.networks[j] = wireless.networks[j - 1];
            j--;
        }
        wireless.networks[j] = held;
    }
}

static void log_networks(void) {
    kernel_log_puts("[wifi] scan: ");
    kernel_log_put_dec(wireless.network_count);
    kernel_log_puts(" network(s) from ");
    kernel_log_put_dec(wireless.frames_seen);
    kernel_log_puts(" frame(s), status ");
    kernel_log_put_dec(wireless.scan_result.status);
    kernel_log_puts("\n");
    for (uint32_t i = 0; i < wireless.network_count; i++) {
        const intel_wireless_network_t *n = &wireless.networks[i];
        kernel_log_puts("[wifi]   ");
        if (n->signal_dbm < 0) {
            kernel_log_puts("-");
            kernel_log_put_dec((uint32_t)(-n->signal_dbm));
        } else {
            kernel_log_put_dec((uint32_t)n->signal_dbm);
        }
        kernel_log_puts(" dBm  ch ");
        kernel_log_put_dec(n->network.channel);
        kernel_log_puts("  ");
        kernel_log_puts(ieee80211_security_name(n->network.security));
        kernel_log_puts("  ");
        kernel_log_puts(n->network.hidden ? "(hidden)" : n->network.ssid);
        kernel_log_puts("\n");
    }
}

int intel_wireless_scan(void) {
    static intel_wireless_scan_request_v17_t request;
    zero(&request, sizeof(request));
    wireless.scan_generation++;
    request.uid = wireless.scan_generation & 0xFFu;
    request.ooc_priority = IWL_SCAN_PRIORITY_EXT_6;

    request.general.flags = IWL_UMAC_SCAN_GEN_FLAGS_V2_PASS_ALL | IWL_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE |
                            IWL_UMAC_SCAN_GEN_FLAGS_V2_ADAPTIVE_DWELL;
    request.general.scan_start_mac_or_link_id = MAC_ID;
    request.general.active_dwell[SCAN_LB_LMAC_IDX] = 10;
    request.general.active_dwell[SCAN_HB_LMAC_IDX] = 10;
    request.general.passive_dwell[SCAN_LB_LMAC_IDX] = 110;
    request.general.passive_dwell[SCAN_HB_LMAC_IDX] = 110;
    request.general.adwell_default_social_channel = 10;
    request.general.adwell_default_2g = 2;
    request.general.adwell_default_5g = 8;
    request.general.adwell_max_budget = 300;
    request.general.scan_priority = IWL_SCAN_PRIORITY_EXT_6;

    request.channel.flags = IWL_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER;
    request.channel.n_aps_override[0] = 10;
    request.channel.n_aps_override[1] = 2;
    uint32_t count = 0;
    for (uint32_t i = 0; i < NVM_CHANNEL_COUNT && count < SCAN_MAX_NUM_CHANS_V3; i++) {
        if (i < wireless.channel_flag_count && !(wireless.channel_flags[i] & NVM_CHANNEL_VALID)) {
            continue;
        }
        if (wireless.channel_flag_count == 0 && i >= 14) {
            break;
        }
        intel_wireless_scan_channel_t *channel = &request.channel.channels[count++];
        channel->channel_number = nvm_channels[i];
        channel->band = nvm_channels[i] > 14 ? PHY_BAND_5 : PHY_BAND_24;
        channel->iteration_count = 1;
        channel->iteration_interval = 0;
    }
    request.channel.count = (uint8_t)count;
    request.periodic.schedule[0].iteration_count = 1;
    request.periodic.schedule[0].interval = 0;

    wireless.network_count = 0;
    wireless.frames_seen = 0;
    wireless.scan_complete = 0;
    zero(&wireless.scan_result, sizeof(wireless.scan_result));
    wireless.state = INTEL_WIRELESS_STATE_SCANNING;
    kernel_log_puts("[wifi] scanning ");
    kernel_log_put_dec(count);
    kernel_log_puts(" channels, listening only\n");

    int result = send(LEGACY_GROUP, SCAN_REQ_UMAC, &request, sizeof(request), 0, 0, 0);
    log_result("scan request", result);
    if (result == INTEL_WIRELESS_OK) {
        result = wait_for(&wireless.scan_complete, SCAN_TIMEOUT_MS);
        log_result("scan", result);
    }
    sort_networks();
    log_networks();
    wireless.state = result == INTEL_WIRELESS_OK ? INTEL_WIRELESS_STATE_READY : INTEL_WIRELESS_STATE_FAILED;
    if (result != INTEL_WIRELESS_OK) {
        log_diagnostics();
    }
    return result;
}

int intel_wireless_state(void) {
    return wireless.state;
}

uint32_t intel_wireless_networks(intel_wireless_network_t *out, uint32_t capacity) {
    if (wireless.state == INTEL_WIRELESS_STATE_SCANNING) {
        return 0;
    }
    uint32_t count = wireless.network_count < capacity ? wireless.network_count : capacity;
    for (uint32_t i = 0; i < count; i++) {
        out[i] = wireless.networks[i];
    }
    return count;
}

int intel_wireless_mac_address(uint8_t out[6]) {
    if (!wireless.have_mac_address) {
        return 0;
    }
    copy_bytes(out, wireless.mac_address, 6);
    return 1;
}

void intel_wireless_request_scan(void) {
    wireless.scan_requested = 1;
}

static uint8_t band_of(uint8_t channel) {
    return channel > 14 ? PHY_BAND_5 : PHY_BAND_24;
}

static int phy_context(uint32_t action) {
    intel_wireless_phy_context_t phy;
    zero(&phy, sizeof(phy));
    phy.id_and_color = PHY_CONTEXT_ID;
    phy.action = action;
    phy.channel.channel = wireless.target.channel;
    phy.channel.band = band_of(wireless.target.channel);
    phy.channel.width = PHY_CHANNEL_WIDTH_20;
    phy.channel.control_position = PHY_CONTROL_POSITION_BELOW;
    phy.lmac_id = 0;
    int result = send(LEGACY_GROUP, PHY_CONTEXT_CMD, &phy, sizeof(phy), 0, 0, 0);
    if (result != INTEL_WIRELESS_OK || action == FW_CTXT_ACTION_REMOVE) {
        return result;
    }
    /* Version 2 of the RLC command is where this firmware takes the receive
       chains: both antennas valid, two of them listening idle and two when
       busy - 0x2806 on this card, which is what Linux sends. */
    intel_wireless_rlc_config_t rlc;
    zero(&rlc, sizeof(rlc));
    rlc.phy_id = PHY_CONTEXT_ID;
    rlc.rx_chain_info = (uint32_t)wireless.rx_antennas << PHY_RX_CHAIN_VALID_POSITION |
                        2u << PHY_RX_CHAIN_COUNT_POSITION | 2u << PHY_RX_CHAIN_MIMO_COUNT_POSITION;
    return send(DATA_PATH_GROUP, RLC_CONFIG_CMD, &rlc, sizeof(rlc), 0, 0, 0);
}

static int binding(uint32_t action) {
    intel_wireless_binding_t command;
    zero(&command, sizeof(command));
    command.id_and_color = PHY_CONTEXT_ID;
    command.action = action;
    command.macs[0] = action == FW_CTXT_ACTION_REMOVE ? FW_CTXT_INVALID : MAC_ID;
    command.macs[1] = FW_CTXT_INVALID;
    command.macs[2] = FW_CTXT_INVALID;
    command.phy = PHY_CONTEXT_ID;
    command.lmac_id = 0;
    return send(LEGACY_GROUP, BINDING_CONTEXT_CMD, &command, sizeof(command), 0, 0, 0);
}

/* The access point as a station in the firmware's table. Aggregation is off
   on every traffic identifier, and the association id goes in once there is
   one. The answer is a status word, and anything but success is a refusal. */
static int station(int modify) {
    intel_wireless_add_station_t command;
    zero(&command, sizeof(command));
    command.add_modify = modify ? STA_MODE_MODIFY : STA_MODE_ADD;
    command.station_id = STATION_ID;
    command.mac_id_n_color = MAC_ID;
    command.tid_disable_tx = 0xFFFF;
    command.station_flags_mask = STA_FLG_FAT_EN_MSK | STA_FLG_MIMO_EN_MSK | STA_FLG_RTS_MIMO_PROT;
    command.station_type = IWL_STA_LINK;
    if (!modify) {
        copy_bytes(command.address, wireless.target.bssid, 6);
    }
    command.association_id = modify ? wireless.association_id : 0;
    uint32_t status = 0;
    uint32_t length = 0;
    int result = send(LEGACY_GROUP, ADD_STA, &command, sizeof(command), &status, sizeof(status), &length);
    if (result == INTEL_WIRELESS_OK && (length < 4 || (status & IWL_ADD_STA_STATUS_MASK) != ADD_STA_SUCCESS)) {
        log_hex("[wifi] ADD_STA status ", status);
        kernel_log_puts("\n");
        return INTEL_WIRELESS_FIRMWARE_ERROR;
    }
    return result;
}

static int remove_station(void) {
    intel_wireless_remove_station_t command;
    zero(&command, sizeof(command));
    command.station_id = STATION_ID;
    return send(LEGACY_GROUP, REMOVE_STA, &command, sizeof(command), 0, 0, 0);
}

static int add_queue(void) {
    intel_wireless_queue_config_t command;
    zero(&command, sizeof(command));
    uint32_t cb_size = 0;
    uint64_t tfds = 0;
    uint64_t byte_counts = 0;
    int result = intel_wireless_data_queue_memory(&tfds, &byte_counts, &cb_size);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    command.tfd_queue_address = tfds;
    command.byte_count_address = byte_counts;
    command.operation = IWL_SCD_QUEUE_ADD;
    command.station_mask = 1u << STATION_ID;
    command.tid = IWL_MGMT_TID;
    command.cb_size = cb_size;
    intel_wireless_queue_config_response_t response;
    zero(&response, sizeof(response));
    uint32_t length = 0;
    result = send(DATA_PATH_GROUP, SCD_QUEUE_CONFIG_CMD, &command, sizeof(command), &response, sizeof(response),
                  &length);
    if (result != INTEL_WIRELESS_OK) {
        return result;
    }
    if (length != sizeof(response)) {
        return INTEL_WIRELESS_FIRMWARE_ERROR;
    }
    intel_wireless_data_queue_start(response.queue_number, response.write_pointer);
    kernel_log_puts("[wifi] transmit queue ");
    kernel_log_put_dec(response.queue_number);
    kernel_log_puts("\n");
    return INTEL_WIRELESS_OK;
}

static int remove_queue(void) {
    intel_wireless_queue_config_t command;
    zero(&command, sizeof(command));
    command.operation = IWL_SCD_QUEUE_REMOVE;
    command.station_mask = 1u << STATION_ID;
    command.tid = IWL_MGMT_TID;
    intel_wireless_data_queue_stop();
    return send(DATA_PATH_GROUP, SCD_QUEUE_CONFIG_CMD, &command, sizeof(command), 0, 0, 0);
}

static int session_protection(uint32_t action) {
    intel_wireless_session_protection_t command;
    zero(&command, sizeof(command));
    command.id_and_color = MAC_ID;
    command.action = action;
    command.configuration = SESSION_PROTECT_CONF_ASSOC;
    command.duration_tu = SESSION_DURATION_TU;
    return send(MAC_CONF_GROUP, SESSION_PROTECTION_CMD, &command, sizeof(command), 0, 0, 0);
}

/* Rate control belongs to the firmware once it knows what the network
   accepts: every legacy rate of the band, no HT - this station associates
   without HT capabilities, so nothing else would be understood. */
static int rate_control(void) {
    intel_wireless_tlc_config_t command;
    zero(&command, sizeof(command));
    command.station_id = STATION_ID;
    command.max_channel_width = IWL_TLC_MNG_CH_WIDTH_20MHZ;
    command.mode = IWL_TLC_MNG_MODE_NON_HT;
    command.chains = wireless.tx_antennas & 3;
    command.non_ht_rates = wireless.target.channel > 14 ? 0x0FF0 : 0x0FFF;
    command.max_mpdu_length = 3839;
    return send(DATA_PATH_GROUP, TLC_MNG_CONFIG_CMD, &command, sizeof(command), 0, 0, 0);
}

/* Before the firmware has rate control for a station - management frames,
   and the handshake's data frames - this family wants the driver to choose:
   the lowest rate of the band, on antenna B, which Bluetooth does not share. */
static uint32_t lowest_rate(void) {
    uint32_t antenna = 2u << RATE_MCS_ANT_POSITION;
    return wireless.target.channel > 14 ? (RATE_MCS_MOD_TYPE_LEGACY_OFDM | antenna)
                                        : (RATE_MCS_MOD_TYPE_CCK | antenna);
}

static int transmit_frame(const uint8_t *frame, uint32_t length, int management) {
    uint32_t header_length = ieee80211_header_length(frame);
    if (length < header_length) {
        return INTEL_WIRELESS_TOO_LARGE;
    }
    if (header_length > 32) {
        return INTEL_WIRELESS_TOO_LARGE;
    }
    intel_wireless_tx_command_t command;
    zero(&command, sizeof(command));
    command.length = (uint16_t)length;
    command.offload_assist = (uint16_t)((header_length / 2) << TX_CMD_OFFLD_MH_SIZE);
    if (header_length % 4) {
        command.offload_assist |= (uint16_t)(1u << TX_CMD_OFFLD_PAD);
    }
    /* The radio encrypts what the header says is protected and leaves room
       for the CCMP header itself - Linux sets the bit and reserves nothing
       on this family. Without it the frame leaves in the clear, the access
       point acknowledges it and then drops it: the handshake completed on
       the laptop and not one DHCP discover was ever answered. */
    uint8_t header[32];
    copy_bytes(header, frame, header_length);
    int encrypt = wireless.pairwise_key && !management;
    if (encrypt) {
        header[1] |= (uint8_t)(IEEE80211_FRAME_CONTROL_PROTECTED >> 8);
    } else {
        command.flags |= IWL_TX_FLAGS_ENCRYPT_DIS;
    }
    if (management || !wireless.authorized) {
        command.flags |= IWL_TX_FLAGS_CMD_RATE | IWL_TX_FLAGS_HIGH_PRI;
        command.rate_n_flags = lowest_rate();
    }
    return intel_wireless_data_send(&command, header, header_length, frame + header_length, length - header_length);
}

static int wait_for_answer(volatile int *flag, uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; waited++) {
        int polled = intel_wireless_poll();
        if (polled != INTEL_WIRELESS_OK) {
            return polled;
        }
        if (*flag) {
            return INTEL_WIRELESS_OK;
        }
        intel_wireless_sleep_ms(1);
    }
    return INTEL_WIRELESS_TIMED_OUT;
}

static void intel_leave(void);

static int fail_join(const char *what, int result, uint16_t *status) {
    log_result(what, result);
    *status = 0xFFFF;
    intel_leave();
    return -1;
}

static int intel_join(const ieee80211_network_t *network, const uint8_t *rsn, uint32_t rsn_length,
                      uint16_t *status) {
    if (wireless.joining || wireless.associated) {
        intel_leave();
    }
    copy_bytes(&wireless.target, network, sizeof(*network));
    wireless.joining = 1;
    wireless.association_answered = 0;
    wireless.authentication_answered = 0;
    wireless.have_sync = 0;
    wireless.sync_dtim_count = network->dtim_count;
    wireless.sync_dtim_period = network->dtim_period;
    wireless.pairwise_key = 0;
    wireless.authorized = 0;
    wireless.last_unicast_pn = 0;
    wireless.last_group_pn = 0;
    wireless.data_frames_received = 0;
    wireless.frames_dropped = 0;
    wireless.transmit_failures = 0;
    wireless.frames_queued_at_join = intel_wireless_transport_statistics()->frames_queued;
    for (uint32_t i = 0; i < wireless.network_count; i++) {
        if (same_address(wireless.networks[i].network.bssid, network->bssid)) {
            wireless.have_sync = wireless.networks[i].sync_device_time != 0;
            wireless.sync_tsf = wireless.networks[i].network.timestamp;
            wireless.sync_device_time = wireless.networks[i].sync_device_time;
        }
    }

    int result = phy_context(FW_CTXT_ACTION_ADD);
    if (result != INTEL_WIRELESS_OK) {
        return fail_join("PHY context", result, status);
    }
    wireless.phy_added = 1;
    /* Linux's order: the channel, then the MAC bound to it, then the BSSID
       it is to listen for. */
    result = binding(FW_CTXT_ACTION_ADD);
    if (result != INTEL_WIRELESS_OK) {
        return fail_join("binding", result, status);
    }
    wireless.binding_added = 1;
    result = mac_context(FW_CTXT_ACTION_MODIFY, network->bssid, 0);
    if (result != INTEL_WIRELESS_OK) {
        return fail_join("station context", result, status);
    }
    result = station(0);
    if (result != INTEL_WIRELESS_OK) {
        return fail_join("access point station", result, status);
    }
    wireless.station_added = 1;
    result = add_queue();
    if (result != INTEL_WIRELESS_OK) {
        return fail_join("transmit queue", result, status);
    }
    wireless.queue_added = 1;

    wireless.session_started = 0;
    result = session_protection(FW_CTXT_ACTION_ADD);
    if (result != INTEL_WIRELESS_OK) {
        return fail_join("session protection", result, status);
    }
    if (wait_for_answer(&wireless.session_started, SESSION_START_TIMEOUT_MS) != INTEL_WIRELESS_OK) {
        kernel_log_puts("[wifi] time on the channel did not start - sending anyway\n");
    }

    static uint8_t frame[512];
    int answered = 0;
    for (int attempt = 0; attempt < JOIN_ATTEMPTS && !answered; attempt++) {
        wireless.authentication_answered = 0;
        uint32_t length = ieee80211_build_authentication(frame, wireless.mac_address, network->bssid);
        transmit_frame(frame, length, 1);
        answered = wait_for_answer(&wireless.authentication_answered, AUTHENTICATION_TIMEOUT_MS) == INTEL_WIRELESS_OK;
    }
    if (!answered || wireless.authentication_status != IEEE80211_STATUS_SUCCESS) {
        kernel_log_puts(answered ? "[wifi] authentication refused\n" : "[wifi] no answer to authentication\n");
        *status = answered ? wireless.authentication_status : 0xFFFF;
        intel_leave();
        return -1;
    }
    kernel_log_puts("[wifi] authenticated\n");

    answered = 0;
    for (int attempt = 0; attempt < JOIN_ATTEMPTS && !answered; attempt++) {
        wireless.association_answered = 0;
        uint32_t length =
            ieee80211_build_association_request(frame, sizeof(frame), wireless.mac_address, network, rsn, rsn_length);
        transmit_frame(frame, length, 1);
        answered = wait_for_answer(&wireless.association_answered, ASSOCIATION_TIMEOUT_MS) == INTEL_WIRELESS_OK;
    }
    if (!answered || wireless.association_status != IEEE80211_STATUS_SUCCESS) {
        kernel_log_puts(answered ? "[wifi] association refused\n" : "[wifi] no answer to association\n");
        *status = answered ? wireless.association_status : 0xFFFF;
        intel_leave();
        return -1;
    }
    kernel_log_puts("[wifi] association id ");
    kernel_log_put_dec(wireless.association_id);
    kernel_log_puts(wireless.have_sync ? "\n" : ", no beacon timing yet\n");

    wireless.associated = 1;
    wireless.joining = 0;
    result = mac_context(FW_CTXT_ACTION_MODIFY, network->bssid, 1);
    if (result == INTEL_WIRELESS_OK) {
        result = station(1);
    }
    if (result == INTEL_WIRELESS_OK) {
        result = rate_control();
    }
    if (result != INTEL_WIRELESS_OK) {
        return fail_join("associated state", result, status);
    }
    wireless.authorized = network->security == IEEE80211_SECURITY_OPEN;
    *status = 0;
    return 0;
}

/* What crossed the radio while joined - the one line that says, from a
   laptop with no serial port, whether a network that gave no address was
   never sent anything, sent things it ignored, or answered and was not
   heard. */
static void log_traffic(void) {
    kernel_log_puts("[wifi] while joined: ");
    kernel_log_put_dec(intel_wireless_transport_statistics()->frames_queued - wireless.frames_queued_at_join);
    kernel_log_puts(" frame(s) sent, ");
    kernel_log_put_dec(wireless.transmit_failures);
    kernel_log_puts(" not delivered, ");
    kernel_log_put_dec(wireless.data_frames_received);
    kernel_log_puts(" data frame(s) received, ");
    kernel_log_put_dec(wireless.frames_dropped);
    kernel_log_puts(" dropped\n");
}

static void intel_leave(void) {
    static uint8_t frame[64];
    if (wireless.associated) {
        log_traffic();
    }
    if (wireless.associated && wireless.queue_added) {
        uint32_t length = ieee80211_build_deauthentication(frame, wireless.mac_address, wireless.target.bssid,
                                                           REASON_LEAVING);
        transmit_frame(frame, length, 1);
        for (int i = 0; i < 20 && intel_wireless_data_in_flight(); i++) {
            intel_wireless_poll();
            intel_wireless_sleep_ms(1);
        }
    }
    wireless.associated = 0;
    wireless.joining = 0;
    wireless.pairwise_key = 0;
    wireless.authorized = 0;
    if (wireless.phy_added) {
        mac_context(FW_CTXT_ACTION_MODIFY, 0, 0);
    }
    if (wireless.queue_added) {
        remove_queue();
        wireless.queue_added = 0;
    }
    if (wireless.station_added) {
        remove_station();
        wireless.station_added = 0;
    }
    if (wireless.phy_added) {
        session_protection(FW_CTXT_ACTION_REMOVE);
    }
    if (wireless.binding_added) {
        binding(FW_CTXT_ACTION_REMOVE);
        wireless.binding_added = 0;
    }
    if (wireless.phy_added) {
        phy_context(FW_CTXT_ACTION_REMOVE);
        wireless.phy_added = 0;
    }
}

/* Everything the firmware held for a network dies with the firmware: after a
   restart there is no PHY, binding, station or queue to take apart, and a
   leave that tried would only send commands about things it never added. */
static void forget_join(void) {
    wireless.joining = 0;
    wireless.associated = 0;
    wireless.phy_added = 0;
    wireless.binding_added = 0;
    wireless.station_added = 0;
    wireless.queue_added = 0;
    wireless.pairwise_key = 0;
    wireless.authorized = 0;
    wireless.session_started = 0;
    wireless.authentication_answered = 0;
    wireless.association_answered = 0;
}

int intel_wireless_restart(void) {
    intel_wireless_stop();
    forget_join();
    if (!wireless.firmware_file) {
        return INTEL_WIRELESS_NOT_READY;
    }
    return intel_wireless_bring_up(wireless.registers, wireless.pci_device_id, wireless.firmware_file,
                                   wireless.firmware_length);
}

/* Keys go in at an offset of the firmware's key table: the pairwise key at
   zero, a group key at one past its own index, so a new group key can arrive
   beside the old one while frames under both are still in the air. */
static int intel_install_key(int group, const uint8_t *key, uint32_t length, uint8_t index) {
    if (length != 16 || !wireless.station_added) {
        return -1;
    }
    intel_wireless_add_station_key_t command;
    zero(&command, sizeof(command));
    command.station_id = STATION_ID;
    command.key_offset = group ? (uint8_t)(1 + (index & 3)) : 0;
    uint16_t key_index = group ? (index & 3) : 0;
    command.key_flags = (uint16_t)(STA_KEY_FLG_CCM | STA_KEY_FLG_WEP_KEY_MAP | key_index << STA_KEY_FLG_KEYID_POSITION |
                                   (group ? STA_KEY_MULTICAST : 0));
    copy_bytes(command.key, key, 16);
    uint32_t status = 0;
    uint32_t answer = 0;
    int result = send(LEGACY_GROUP, ADD_STA_KEY, &command, sizeof(command), &status, sizeof(status), &answer);
    zero(&command, sizeof(command));
    log_result(group ? "group key" : "pairwise key", result);
    if (result != INTEL_WIRELESS_OK || answer < 4 || (status & IWL_ADD_STA_STATUS_MASK) != ADD_STA_SUCCESS) {
        return -1;
    }
    if (group) {
        wireless.last_group_pn = 0;
    } else {
        wireless.pairwise_key = 1;
        wireless.authorized = 1;
    }
    return 0;
}

static int intel_transmit(const uint8_t *ethernet, uint32_t length) {
    if (!wireless.associated) {
        return -1;
    }
    static uint8_t frame[2048];
    uint32_t frame_length = ieee80211_data_from_ethernet(ethernet, length, wireless.target.bssid, frame, sizeof(frame));
    if (frame_length == 0) {
        return -1;
    }
    return transmit_frame(frame, frame_length, 0) == INTEL_WIRELESS_OK ? 0 : -1;
}

static int intel_scan(void) {
    int result = intel_wireless_scan();
    for (uint32_t i = 0; i < wireless.network_count; i++) {
        wireless_heard(&wireless.networks[i].network, wireless.networks[i].signal_dbm);
    }
    return result == INTEL_WIRELESS_OK ? 0 : -1;
}

static int intel_service(void) {
    int polled = intel_wireless_poll();
    if (polled != INTEL_WIRELESS_OK) {
        fail("the running firmware", polled);
        return -1;
    }
    return 0;
}

static wireless_backend_t intel_backend = {
    "intel", {0}, intel_scan, intel_join, intel_transmit, intel_install_key, intel_leave, intel_service,
};

const wireless_backend_t *intel_wireless_backend(void) {
    copy_bytes(intel_backend.address, wireless.mac_address, 6);
    return &intel_backend;
}

#ifndef LEANOS_HOST_TEST
static const uint16_t known_devices[] = {0xA0F0, 0x43F0, 0x02F0, 0x06F0, 0x34F0, 0x3DF0, 0x4DF0};

static int find_device(pci_device_t *out) {
    for (uint32_t i = 0; i < sizeof(known_devices) / sizeof(known_devices[0]); i++) {
        if (pci_find_device(INTEL_VENDOR, known_devices[i], out)) {
            return 1;
        }
    }
    return 0;
}

static uint64_t physical_address_limit(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u));
    if (eax < 0x80000008u) {
        return 1ULL << 36;
    }
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000008u));
    return 1ULL << (eax & 0xFF);
}

static uint8_t *load_firmware(const char *name, uint32_t *length) {
    char path[96];
    const char *prefix = "/lib/firmware/";
    uint32_t at = 0;
    for (const char *c = prefix; *c; c++) {
        path[at++] = *c;
    }
    for (const char *c = name; *c && at + 1 < sizeof(path); c++) {
        path[at++] = *c;
    }
    path[at] = 0;
    leanfs_stat_t st;
    if (virtual_file_system_stat(path, &st) != 0 || st.is_directory || st.size == 0) {
        kernel_log_puts("[wifi] ");
        kernel_log_puts(path);
        kernel_log_puts(" is not on this disk - tools/make-hardware-image.sh puts it there\n");
        return 0;
    }
    uint8_t *bytes = (uint8_t *)kmalloc(st.size);
    if (!bytes) {
        return 0;
    }
    if (virtual_file_system_read(path, bytes, st.size) < 0) {
        kfree(bytes);
        return 0;
    }
    *length = (uint32_t)st.size;
    return bytes;
}

static void bring_up_and_run(void) {
    pci_device_t device;
    if (!find_device(&device)) {
        wireless.state = INTEL_WIRELESS_STATE_ABSENT;
        return;
    }
    kernel_log_puts("[wifi] Intel wireless ");
    kernel_log_put_hex32(device.device_id);
    kernel_log_puts(" at ");
    kernel_log_put_hex32((uint32_t)device.bus << 8 | (uint32_t)device.slot << 3 | device.func);
    kernel_log_puts("\n");

    uint64_t base = pci_bar_memory_base(&device, 0);
    uint64_t size = pci_bar_memory_size(&device, 0);
    if (base == 0 && size) {
        uint64_t flags = irq_save_disable();
        base = pci_assign_memory_bar(&device, 0, physical_address_limit());
        irq_restore(flags);
        log_hex("[wifi] BAR0 was unassigned; placed at ", (uint32_t)(base >> 32));
        log_hex(":", (uint32_t)base);
        kernel_log_puts("\n");
    }
    if (!base || size < 0x2000) {
        kernel_log_puts("[wifi] no usable register window\n");
        wireless.state = INTEL_WIRELESS_STATE_FAILED;
        return;
    }
    pci_set_power_state_d0(&device);
    pci_enable_device(&device);
    pci_disable_legacy_interrupt(&device);
    volatile uint8_t *registers = (volatile uint8_t *)virtual_memory_map_mmio(base, size);
    if (!registers) {
        wireless.state = INTEL_WIRELESS_STATE_FAILED;
        return;
    }
    intel_wireless_transport_attach(registers);

    wireless.hardware_revision = intel_wireless_read32(CSR_HW_REV);
    if (wireless.hardware_revision == 0xFFFFFFFFu) {
        kernel_log_puts("[wifi] the device does not answer (revision reads all ones)\n");
        wireless.state = INTEL_WIRELESS_STATE_FAILED;
        return;
    }
    int prepared = intel_wireless_prepare_card();
    if (prepared == INTEL_WIRELESS_OK) {
        intel_wireless_write32(CSR_GP_CNTRL, intel_wireless_read32(CSR_GP_CNTRL) | CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
        intel_wireless_delay_us(2000);
    }
    wireless.rf_id = intel_wireless_read32(CSR_HW_RF_ID);
    uint32_t crf = intel_wireless_read_periphery(SD_REG_VER);
    uint32_t cnv = intel_wireless_read_periphery(CNVI_AUX_MISC_CHIP);
    kernel_log_puts("[wifi]");
    log_hex(" revision ", wireless.hardware_revision);
    log_hex(" rf ", wireless.rf_id);
    log_hex(" crf ", crf);
    log_hex(" cnv ", cnv);
    kernel_log_puts(prepared == INTEL_WIRELESS_OK ? ", the device is ours\n" : ", the device would not hand itself over\n");

    char name[64];
    if (!intel_wireless_firmware_name(wireless.hardware_revision, wireless.rf_id, name, sizeof(name))) {
        kernel_log_puts("[wifi] no firmware this driver knows fits that revision and radio\n");
        wireless.state = INTEL_WIRELESS_STATE_FAILED;
        return;
    }
    uint32_t length = 0;
    uint8_t *file = load_firmware(name, &length);
    if (!file) {
        wireless.state = INTEL_WIRELESS_STATE_FAILED;
        return;
    }
    kernel_log_puts("[wifi] loading ");
    kernel_log_puts(name);
    kernel_log_puts("\n");
    if (intel_wireless_bring_up(registers, device.device_id, file, length) != INTEL_WIRELESS_OK) {
        return;
    }
    for (uint32_t restarts = 0;; restarts++) {
        wireless_run(intel_wireless_backend());
        if (restarts == FIRMWARE_RESTARTS_MAX) {
            break;
        }
        kernel_log_puts("[wifi] the firmware stopped - loading it again\n");
        wireless_set_starting();
        if (intel_wireless_restart() != INTEL_WIRELESS_OK) {
            break;
        }
    }
    const intel_wireless_transport_statistics_t *statistics = intel_wireless_transport_statistics();
    kernel_log_puts("[wifi] the radio stopped after ");
    kernel_log_put_dec(statistics->frames_queued);
    kernel_log_puts(" frame(s) sent\n");
}

/* Whatever stopped the radio - no firmware on the disk, a device that would
   not start, a firmware that died running - the wireless state says so, so
   the picker can say so rather than wait. */
static void wireless_task(void *argument) {
    (void)argument;
    bring_up_and_run();
    wireless_set_failed();
}

int intel_wireless_start(void) {
    pci_device_t device;
    if (!find_device(&device)) {
        return 0;
    }
    wireless_set_starting();
    task_t *task = task_spawn("wifi", wireless_task, 0);
    if (task) {
        task->parent_id = -1;
    }
    return 1;
}
#endif
