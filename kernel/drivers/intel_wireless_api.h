#pragma once

#include <stddef.h>
#include <stdint.h>

/* The structures this driver and Intel's firmware exchange. Every one is
   written here rather than copied, and every one is checked field by field
   against Linux's fw/api headers (dual GPL/BSD) by tools/intel-wireless-
   layout-test.sh - a wrong offset in a command is not an error the firmware
   reports, it is a radio doing something else. Little-endian throughout,
   which this machine is. */

#define INTEL_WIRELESS_PACKED __attribute__((packed))

enum {
    LEGACY_GROUP = 0x0,
    LONG_GROUP = 0x1,
    SYSTEM_GROUP = 0x2,
    MAC_CONF_GROUP = 0x3,
    PHY_OPS_GROUP = 0x4,
    DATA_PATH_GROUP = 0x5,
    SCAN_GROUP = 0x6,
    REGULATORY_AND_NVM_GROUP = 0xC,
};

enum {
    UCODE_ALIVE_NTFY = 0x01,
    REPLY_ERROR = 0x02,
    INIT_COMPLETE_NOTIF = 0x04,
    PHY_CONTEXT_CMD = 0x08,
    SCAN_CFG_CMD = 0x0C,
    SCAN_REQ_UMAC = 0x0D,
    SCAN_ABORT_UMAC = 0x0E,
    SCAN_COMPLETE_UMAC = 0x0F,
    ADD_STA_KEY = 0x17,
    ADD_STA = 0x18,
    TX_CMD = 0x1C,
    MAC_CONTEXT_CMD = 0x28,
    BINDING_CONTEXT_CMD = 0x2B,
    POWER_TABLE_CMD = 0x77,
    TX_ANT_CONFIGURATION_CMD = 0x98,
    BT_CONFIG = 0x9B,
    SCAN_ITERATION_COMPLETE_UMAC = 0xB5,
    REPLY_RX_MPDU_CMD = 0xC1,
    MCC_UPDATE_CMD = 0xC8,
    MCC_CHUB_UPDATE_CMD = 0xC9,
};

enum {
    INIT_EXTENDED_CFG_CMD = 0x03,
    DQA_ENABLE_CMD = 0x00,
    NVM_ACCESS_COMPLETE = 0x00,
    NVM_GET_INFO = 0x02,
};

#define IWL_UCODE_TLV_CAPA_LAR_SUPPORT             1
#define IWL_UCODE_TLV_CAPA_UMAC_SCAN               2
#define IWL_UCODE_TLV_CAPA_DQA_SUPPORT             12
#define IWL_UCODE_TLV_CAPA_LAR_MULTI_MCC           29
#define IWL_UCODE_TLV_CAPA_BT_MPLUT_SUPPORT        67
#define IWL_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT 89

#define IWL_UCODE_TLV_API_ADAPTIVE_DWELL     32
#define IWL_UCODE_TLV_API_NEW_RX_STATS       35
#define IWL_UCODE_TLV_API_ADAPTIVE_DWELL_V2  42
#define IWL_UCODE_TLV_API_REDUCED_SCAN_CONFIG 56
#define IWL_UCODE_TLV_API_SCAN_EXT_CHAN_VER  58
#define IWL_UCODE_TLV_API_BAND_IN_RX_DATA    59

#define SEQ_RX_FRAME 0x8000u
#define QUEUE_TO_SEQUENCE(queue) (((queue) & 0x1Fu) << 8)
#define INDEX_TO_SEQUENCE(index) ((index) & 0xFFu)
#define SEQUENCE_TO_INDEX(sequence) ((sequence) & 0xFFu)

#define FH_RSCSR_FRAME_SIZE_MASK 0x00003FFFu
#define FH_RSCSR_FRAME_INVALID   0x55550000u
#define FH_RSCSR_FRAME_ALIGN     0x40u

typedef struct {
    uint8_t command;
    uint8_t group;
    uint16_t sequence;
    uint16_t length;
    uint8_t reserved;
    uint8_t version;
} INTEL_WIRELESS_PACKED intel_wireless_command_header_t;
_Static_assert(sizeof(intel_wireless_command_header_t) == 8, "iwl_cmd_header_wide");

typedef struct {
    uint32_t length_and_flags;
    uint8_t command;
    uint8_t group;
    uint16_t sequence;
    uint8_t data[];
} INTEL_WIRELESS_PACKED intel_wireless_rx_packet_t;
_Static_assert(sizeof(intel_wireless_rx_packet_t) == 8, "iwl_rx_packet");

typedef struct {
    uint16_t closed_rb_number;
    uint16_t closed_fr_number;
    uint16_t finished_rb_number;
    uint16_t finished_fr_number;
    uint32_t spare;
} INTEL_WIRELESS_PACKED intel_wireless_rb_status_t;
_Static_assert(sizeof(intel_wireless_rb_status_t) == 12, "iwl_rb_status");

typedef struct {
    uint16_t length;
    uint64_t address;
} INTEL_WIRELESS_PACKED intel_wireless_transfer_buffer_t;

#define INTEL_WIRELESS_TFD_BUFFERS 25

typedef struct {
    uint16_t buffer_count;
    intel_wireless_transfer_buffer_t buffers[INTEL_WIRELESS_TFD_BUFFERS];
    uint32_t padding;
} INTEL_WIRELESS_PACKED intel_wireless_tfd_t;
_Static_assert(sizeof(intel_wireless_tfd_t) == 256, "iwl_tfh_tfd");

#define INTEL_WIRELESS_DRAM_ENTRIES 64

#define IWL_CTXT_INFO_TFD_FORMAT_LONG 0x0100u
#define IWL_CTXT_INFO_RB_CB_SIZE_POSITION 4
#define IWL_CTXT_INFO_RB_SIZE_POSITION 9
#define IWL_CTXT_INFO_RB_SIZE_4K 0x4u

typedef struct {
    uint16_t mac_id;
    uint16_t version;
    uint16_t size;
    uint16_t reserved;
    uint32_t control_flags;
    uint32_t control_reserved;
    uint64_t reserved0;
    uint64_t free_rbd_address;
    uint64_t used_rbd_address;
    uint64_t status_write_pointer;
    uint64_t command_queue_address;
    uint8_t command_queue_size;
    uint8_t command_queue_reserved[7];
    uint32_t reserved1[4];
    uint64_t core_dump_address;
    uint32_t core_dump_size;
    uint32_t core_dump_reserved;
    uint64_t early_debug_address;
    uint32_t early_debug_size;
    uint32_t early_debug_reserved;
    uint64_t platform_nvm_address;
    uint32_t platform_nvm_size;
    uint32_t platform_nvm_reserved;
    uint32_t reserved2[16];
    uint64_t umac_image[INTEL_WIRELESS_DRAM_ENTRIES];
    uint64_t lmac_image[INTEL_WIRELESS_DRAM_ENTRIES];
    uint64_t virtual_image[INTEL_WIRELESS_DRAM_ENTRIES];
    uint32_t reserved3[16];
} INTEL_WIRELESS_PACKED intel_wireless_context_info_t;
_Static_assert(sizeof(intel_wireless_context_info_t) == 1792, "iwl_context_info");
_Static_assert(offsetof(intel_wireless_context_info_t, free_rbd_address) == 24, "rbd_cfg");
_Static_assert(offsetof(intel_wireless_context_info_t, command_queue_address) == 48, "hcmd_cfg");
_Static_assert(offsetof(intel_wireless_context_info_t, umac_image) == 192, "dram");

typedef struct {
    uint32_t error_event_table_pointer;
    uint32_t log_event_table_pointer;
    uint32_t cpu_register_pointer;
    uint32_t debug_configuration_pointer;
    uint32_t alive_counter_pointer;
    uint32_t scheduler_base_pointer;
    uint32_t store_forward_address;
    uint32_t store_forward_size;
} INTEL_WIRELESS_PACKED intel_wireless_lmac_debug_t;

typedef struct {
    uint32_t ucode_major;
    uint32_t ucode_minor;
    uint8_t version_subtype;
    uint8_t version_type;
    uint8_t mac;
    uint8_t option;
    uint32_t timestamp;
    intel_wireless_lmac_debug_t debug;
} INTEL_WIRELESS_PACKED intel_wireless_lmac_alive_t;

typedef struct {
    uint32_t umac_major;
    uint32_t umac_minor;
    uint32_t error_info_address;
    uint32_t debug_print_buffer_address;
} INTEL_WIRELESS_PACKED intel_wireless_umac_alive_t;

/* UCODE_ALIVE_NTFY_API_S_VER_6, which is what version 77 of the 22000
   firmware sends - the notification table in the file says so. */
typedef struct {
    uint16_t status;
    uint16_t flags;
    intel_wireless_lmac_alive_t lmac[2];
    intel_wireless_umac_alive_t umac;
    uint32_t sku_id[3];
    uint64_t imr_base_address;
    uint32_t imr_size;
    uint32_t imr_enabled;
} INTEL_WIRELESS_PACKED intel_wireless_alive_v6_t;
_Static_assert(sizeof(intel_wireless_alive_v6_t) == 4 + 2 * 48 + 16 + 12 + 16, "iwl_alive_ntf_v7");

#define IWL_ALIVE_STATUS_OK 0xCAFEu

#define IWL_INIT_NVM 1

typedef struct {
    uint32_t init_flags;
} INTEL_WIRELESS_PACKED intel_wireless_init_extended_config_t;

typedef struct {
    uint32_t reserved;
} INTEL_WIRELESS_PACKED intel_wireless_nvm_access_complete_t;

typedef struct {
    uint32_t reserved;
} INTEL_WIRELESS_PACKED intel_wireless_nvm_get_info_t;

#define IWL_NUM_CHANNELS_V2 110

typedef struct {
    uint32_t flags;
    uint16_t nvm_version;
    uint8_t board_type;
    uint8_t hardware_addresses;
    uint32_t mac_sku_flags;
    uint32_t tx_chains;
    uint32_t rx_chains;
    uint32_t lar_enabled;
    uint32_t channel_count;
    uint32_t channel_profile[IWL_NUM_CHANNELS_V2];
} INTEL_WIRELESS_PACKED intel_wireless_nvm_get_info_response_v4_t;
_Static_assert(sizeof(intel_wireless_nvm_get_info_response_v4_t) == 8 + 4 + 8 + 8 + 440, "iwl_nvm_get_info_rsp_v4");

#define NVM_GENERAL_FLAGS_EMPTY_OTP 0x1u
#define NVM_MAC_SKU_FLAGS_BAND_2_4_ENABLED 0x1u
#define NVM_MAC_SKU_FLAGS_BAND_5_2_ENABLED 0x2u

typedef struct {
    uint32_t valid;
} INTEL_WIRELESS_PACKED intel_wireless_tx_antenna_config_t;

#define BT_COEX_NW 0x1u
#define BT_COEX_MPLUT_ENABLED 0x1u
#define BT_COEX_HIGH_BAND_RET 0x10u

typedef struct {
    uint32_t mode;
    uint32_t enabled_modules;
} INTEL_WIRELESS_PACKED intel_wireless_bt_coex_t;

typedef struct {
    uint32_t command_queue;
} INTEL_WIRELESS_PACKED intel_wireless_dqa_enable_t;

#define MCC_SOURCE_OLD_FW 0
#define MCC_SOURCE_GET_CURRENT 0x10

typedef struct {
    uint16_t mcc;
    uint8_t source_id;
    uint8_t reserved;
    uint32_t key;
    uint8_t reserved2[20];
} INTEL_WIRELESS_PACKED intel_wireless_mcc_update_t;
_Static_assert(sizeof(intel_wireless_mcc_update_t) == 28, "iwl_mcc_update_cmd");

typedef struct {
    uint32_t status;
    uint16_t mcc;
    uint16_t capabilities;
    uint16_t time;
    uint16_t geo_info;
    uint8_t source_id;
    uint8_t reserved[3];
    uint32_t channel_count;
    uint32_t channels[];
} INTEL_WIRELESS_PACKED intel_wireless_mcc_update_response_v4_t;
_Static_assert(sizeof(intel_wireless_mcc_update_response_v4_t) == 20, "iwl_mcc_update_resp_v4");

typedef struct {
    uint32_t error_type;
    uint8_t command;
    uint8_t reserved1;
    uint16_t bad_sequence;
    uint32_t error_info;
    uint64_t timestamp;
} INTEL_WIRELESS_PACKED intel_wireless_error_response_t;

#define SOC_CONFIGURATION_CMD 0x01
#define SOC_CONFIG_CMD_FLAGS_DISCRETE    0x1u
#define SOC_CONFIG_CMD_FLAGS_LOW_LATENCY 0x2u
#define SOC_FLAGS_LTR_APPLY_DELAY_POSITION 2
#define IWL_UCODE_TLV_CAPA_SOC_LATENCY_SUPPORT 37

typedef struct {
    uint32_t flags;
    uint32_t latency;
} INTEL_WIRELESS_PACKED intel_wireless_soc_configuration_t;

enum {
    FW_CTXT_ACTION_ADD = 1,
    FW_CTXT_ACTION_MODIFY = 2,
    FW_CTXT_ACTION_REMOVE = 3,
};

#define FW_MAC_TYPE_BSS_STA 5
#define FW_CTXT_COLOR_POSITION 8

#define MAC_FILTER_ACCEPT_GRP 0x00000004u
#define MAC_FILTER_IN_BEACON  0x00000040u

typedef struct {
    uint16_t cw_min;
    uint16_t cw_max;
    uint8_t aifsn;
    uint8_t fifos_mask;
    uint16_t edca_txop;
} INTEL_WIRELESS_PACKED intel_wireless_ac_qos_t;

typedef struct {
    uint32_t is_assoc;
    uint32_t dtim_time;
    uint64_t dtim_tsf;
    uint32_t beacon_interval;
    uint32_t reserved1;
    uint32_t dtim_interval;
    uint32_t data_policy;
    uint32_t listen_interval;
    uint32_t assoc_id;
    uint32_t assoc_beacon_arrive_time;
} INTEL_WIRELESS_PACKED intel_wireless_mac_data_station_t;

#define INTEL_WIRELESS_AC_COUNT 4

/* MAC_CONTEXT_CMD. The union in Linux's struct is as large as its largest
   member (the GO's), which is what the firmware reads; the station's part is
   at the front of it and the rest stays zero. */
#define INTEL_WIRELESS_MAC_UNION_BYTES 48

typedef struct {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t mac_type;
    uint32_t tsf_id;
    uint8_t node_address[6];
    uint16_t reserved_for_node_address;
    uint8_t bssid_address[6];
    uint16_t reserved_for_bssid_address;
    uint32_t cck_rates;
    uint32_t ofdm_rates;
    uint32_t protection_flags;
    uint32_t cck_short_preamble;
    uint32_t short_slot;
    uint32_t filter_flags;
    uint32_t qos_flags;
    intel_wireless_ac_qos_t ac[INTEL_WIRELESS_AC_COUNT + 1];
    union {
        intel_wireless_mac_data_station_t station;
        uint8_t bytes[INTEL_WIRELESS_MAC_UNION_BYTES];
    };
} INTEL_WIRELESS_PACKED intel_wireless_mac_context_t;

typedef struct {
    uint8_t enable_cam_mode;
    uint8_t enable_promiscuous_mode;
    uint8_t broadcast_station_id;
    uint8_t reserved;
    uint32_t tx_chains;
    uint32_t rx_chains;
} INTEL_WIRELESS_PACKED intel_wireless_scan_config_t;
_Static_assert(sizeof(intel_wireless_scan_config_t) == 12, "iwl_scan_config");

#define SCAN_TWO_LMACS 2
#define SCAN_LB_LMAC_IDX 0
#define SCAN_HB_LMAC_IDX 1
#define SCAN_MAX_NUM_CHANS_V3 67
#define SCAN_NUM_BAND_PROBE_DATA_V_2 3
#define SCAN_OFFLOAD_PROBE_REQ_SIZE 512
#define PROBE_OPTION_MAX 20
#define SCAN_SHORT_SSID_MAX_SIZE 8
#define SCAN_BSSID_MAX_SIZE 16
#define IWL_MAX_SCHED_SCAN_PLANS 2
#define IEEE80211_MAX_SSID_LEN 32

#define IWL_UMAC_SCAN_GEN_FLAGS_V2_PASS_ALL           0x0002u
#define IWL_UMAC_SCAN_GEN_FLAGS_V2_NTFY_ITER_COMPLETE 0x0004u
#define IWL_UMAC_SCAN_GEN_FLAGS_V2_ADAPTIVE_DWELL     0x0080u
#define IWL_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE      0x0800u

#define IWL_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER 0x20u
#define IWL_SCAN_PRIORITY_EXT_6 6

#define PHY_BAND_5  0
#define PHY_BAND_24 1

typedef struct {
    uint16_t flags;
    uint8_t reserved;
    uint8_t scan_start_mac_or_link_id;
    uint8_t active_dwell[SCAN_TWO_LMACS];
    uint8_t adwell_default_2g;
    uint8_t adwell_default_5g;
    uint8_t adwell_default_social_channel;
    uint8_t flags2;
    uint16_t adwell_max_budget;
    uint32_t max_out_of_time[SCAN_TWO_LMACS];
    uint32_t suspend_time[SCAN_TWO_LMACS];
    uint32_t scan_priority;
    uint8_t passive_dwell[SCAN_TWO_LMACS];
    uint8_t num_of_fragments[SCAN_TWO_LMACS];
} INTEL_WIRELESS_PACKED intel_wireless_scan_general_v11_t;

typedef struct {
    uint32_t flags;
    uint8_t channel_number;
    uint8_t band;
    uint8_t iteration_count;
    uint8_t iteration_interval;
} INTEL_WIRELESS_PACKED intel_wireless_scan_channel_t;
_Static_assert(sizeof(intel_wireless_scan_channel_t) == 8, "iwl_scan_channel_cfg_umac");

typedef struct {
    uint8_t flags;
    uint8_t count;
    uint8_t n_aps_override[2];
    intel_wireless_scan_channel_t channels[SCAN_MAX_NUM_CHANS_V3];
} INTEL_WIRELESS_PACKED intel_wireless_scan_channel_params_v7_t;

typedef struct {
    uint16_t interval;
    uint8_t iteration_count;
    uint8_t reserved;
} INTEL_WIRELESS_PACKED intel_wireless_scan_schedule_t;

typedef struct {
    intel_wireless_scan_schedule_t schedule[IWL_MAX_SCHED_SCAN_PLANS];
    uint16_t delay;
    uint16_t reserved;
} INTEL_WIRELESS_PACKED intel_wireless_scan_periodic_t;

typedef struct {
    uint16_t offset;
    uint16_t length;
} INTEL_WIRELESS_PACKED intel_wireless_probe_segment_t;

typedef struct {
    intel_wireless_probe_segment_t mac_header;
    intel_wireless_probe_segment_t band_data[SCAN_NUM_BAND_PROBE_DATA_V_2];
    intel_wireless_probe_segment_t common_data;
    uint8_t buffer[SCAN_OFFLOAD_PROBE_REQ_SIZE];
} INTEL_WIRELESS_PACKED intel_wireless_probe_request_t;

typedef struct {
    uint8_t id;
    uint8_t length;
    uint8_t ssid[IEEE80211_MAX_SSID_LEN];
} INTEL_WIRELESS_PACKED intel_wireless_ssid_ie_t;

typedef struct {
    intel_wireless_probe_request_t probe_request;
    uint8_t short_ssid_count;
    uint8_t bssid_count;
    uint16_t reserved;
    intel_wireless_ssid_ie_t direct_scan[PROBE_OPTION_MAX];
    uint32_t short_ssid[SCAN_SHORT_SSID_MAX_SIZE];
    uint8_t bssid_array[SCAN_BSSID_MAX_SIZE][6];
} INTEL_WIRELESS_PACKED intel_wireless_scan_probe_params_v4_t;

/* SCAN_REQUEST_CMD_UMAC_API_S_VER_17, which versions 14 to 17 share. */
typedef struct {
    uint32_t uid;
    uint32_t ooc_priority;
    intel_wireless_scan_general_v11_t general;
    intel_wireless_scan_channel_params_v7_t channel;
    intel_wireless_scan_periodic_t periodic;
    intel_wireless_scan_probe_params_v4_t probe;
} INTEL_WIRELESS_PACKED intel_wireless_scan_request_v17_t;

typedef struct {
    uint32_t uid;
    uint8_t last_schedule;
    uint8_t last_iteration;
    uint8_t status;
    uint8_t ebs_status;
    uint32_t time_from_last_iteration;
    uint32_t reserved;
} INTEL_WIRELESS_PACKED intel_wireless_scan_complete_t;

typedef struct {
    uint16_t mpdu_length;
    uint8_t mac_flags1;
    uint8_t mac_flags2;
    uint8_t amsdu_info;
    uint16_t phy_info;
    uint8_t mac_phy_band;
    uint16_t raw_checksum;
    uint16_t l3l4_flags;
    uint32_t status;
    uint32_t reorder_data;
    uint32_t rss_hash;
    uint32_t filter_match;
    uint32_t rate_and_flags;
    uint8_t energy_a;
    uint8_t energy_b;
    uint8_t channel;
    uint8_t mac_context;
    uint32_t gp2_on_air_rise;
    uint64_t tsf_on_air_rise;
} INTEL_WIRELESS_PACKED intel_wireless_rx_mpdu_v1_t;
_Static_assert(sizeof(intel_wireless_rx_mpdu_v1_t) == 48, "IWL_RX_DESC_SIZE_V1");

#define IWL_RX_MPDU_STATUS_CRC_OK     0x1u
#define IWL_RX_MPDU_STATUS_OVERRUN_OK 0x2u
#define IWL_RX_MPDU_MFLG2_PAD         0x20u
