#include "intel_wireless.h"

#include "drivers/intel_wireless_api.h"
#include "drivers/intel_wireless_firmware.h"
#include "drivers/intel_wireless_registers.h"
#include "drivers/intel_wireless_transport.h"
#include "drivers/kernel_log.h"
#include "memory_management/heap.h"
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
    ieee80211_network_t network;
    if (ieee80211_parse_beacon(payload + sizeof(*descriptor), length, &network)) {
        remember_network(&network, descriptor);
    }
}

static void handle_packet(const intel_wireless_rx_packet_t *packet, const uint8_t *payload,
                          uint32_t payload_length) {
    int legacy = packet->group <= LONG_GROUP;
    if (legacy && packet->command == INIT_COMPLETE_NOTIF) {
        wireless.init_complete = 1;
    } else if (legacy && packet->command == REPLY_RX_MPDU_CMD) {
        handle_frame(payload, payload_length);
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

static int add_mac_context(void) {
    static intel_wireless_mac_context_t context;
    zero(&context, sizeof(context));
    context.id_and_color = MAC_ID | (0u << FW_CTXT_COLOR_POSITION);
    context.action = FW_CTXT_ACTION_ADD;
    context.mac_type = FW_MAC_TYPE_BSS_STA;
    context.tsf_id = 0;
    copy_bytes(context.node_address, wireless.mac_address, 6);
    for (int i = 0; i < 6; i++) {
        context.bssid_address[i] = 0xFF;
    }
    context.cck_rates = 0x0F;
    context.ofdm_rates = 0x15;
    context.filter_flags = MAC_FILTER_ACCEPT_GRP | MAC_FILTER_IN_BEACON;
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
    }
    context.station.is_assoc = 0;
    context.station.listen_interval = 10;
    return send(LEGACY_GROUP, MAC_CONTEXT_CMD, &context, sizeof(context), 0, 0, 0);
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

static void wireless_task(void *argument) {
    (void)argument;
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
    if (intel_wireless_bring_up(registers, device.device_id, file, length) == INTEL_WIRELESS_OK) {
        intel_wireless_scan();
    }
    if (wireless.state == INTEL_WIRELESS_STATE_FAILED) {
        return;
    }
    for (;;) {
        if (wireless.scan_requested) {
            wireless.scan_requested = 0;
            intel_wireless_scan();
        }
        int polled = intel_wireless_poll();
        if (polled != INTEL_WIRELESS_OK) {
            fail("the running firmware", polled);
            return;
        }
        scheduler_sleep_ms(100);
    }
}

void intel_wireless_start(void) {
    pci_device_t device;
    if (!find_device(&device)) {
        return;
    }
    task_t *task = task_spawn("wifi", wireless_task, 0);
    if (task) {
        task->parent_id = -1;
    }
}
#endif
