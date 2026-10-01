#include "intel_wireless_firmware.h"

#define HEADER_LENGTH 88

#define RECORD_PROBE_MAX_LENGTH   6
#define RECORD_FLAGS              18
#define RECORD_SECTION_RUNTIME    19
#define RECORD_PHY_SKU            23
#define RECORD_SECURE_SECTION_RUNTIME 24
#define RECORD_API_CHANGES_SET    29
#define RECORD_ENABLED_CAPABILITIES 30
#define RECORD_SCAN_CHANNELS      31
#define RECORD_FIRMWARE_VERSION   36
#define RECORD_COMMAND_VERSIONS   48
#define RECORD_STATIONS           0x100

#define PHY_CONFIG_TX_CHAIN_POSITION 16
#define PHY_CONFIG_RX_CHAIN_POSITION 20

#define MAC_TYPE_QU  0x33
#define MAC_TYPE_QUZ 0x35

#define RF_TYPE_HR2 0x10A
#define RF_TYPE_HR1 0x10C

#define LONG_GROUP 1

static uint32_t read32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void clear(void *pointer, uint32_t length) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (uint32_t i = 0; i < length; i++) {
        bytes[i] = 0;
    }
}

/* Runtime sections arrive in one list: the LMAC's, a separator, the UMAC's,
   another separator, and the pages the firmware swaps in from host memory.
   The separators are records of their own with a recognisable address and
   nothing else in them. */
static int store_section(intel_wireless_firmware_t *out, int *phase, const uint8_t *data, uint32_t length) {
    if (length < 4) {
        return -20;
    }
    uint32_t device_offset = read32(data);
    if (device_offset == INTEL_WIRELESS_LMAC_UMAC_SEPARATOR) {
        if (*phase != 0) {
            return -21;
        }
        *phase = 1;
        return 0;
    }
    if (device_offset == INTEL_WIRELESS_PAGING_SEPARATOR) {
        if (*phase != 1) {
            return -22;
        }
        *phase = 2;
        return 0;
    }
    intel_wireless_section_t *list = *phase == 0 ? out->lmac : (*phase == 1 ? out->umac : out->paging);
    uint32_t *count = *phase == 0 ? &out->lmac_count : (*phase == 1 ? &out->umac_count : &out->paging_count);
    if (*count >= INTEL_WIRELESS_MAX_SECTIONS) {
        return -23;
    }
    if (length - 4 == 0) {
        return -24;
    }
    list[*count].device_offset = device_offset;
    list[*count].data = data + 4;
    list[*count].length = length - 4;
    (*count)++;
    return 0;
}

int intel_wireless_firmware_parse(const uint8_t *bytes, uint32_t length, intel_wireless_firmware_t *out) {
    if (!bytes || !out) {
        return -1;
    }
    clear(out, sizeof(*out));
    if (length < HEADER_LENGTH) {
        return -2;
    }
    if (read32(bytes) != 0) {
        return -3;
    }
    if (read32(bytes + 4) != INTEL_WIRELESS_FIRMWARE_MAGIC) {
        return -4;
    }
    for (uint32_t i = 0; i < 64; i++) {
        char c = (char)bytes[8 + i];
        if (c == 0) {
            break;
        }
        out->human_readable[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out->api_version = read32(bytes + 72);

    int phase = 0;
    uint32_t offset = HEADER_LENGTH;
    while (offset < length) {
        if (length - offset < 8) {
            return -5;
        }
        uint32_t type = read32(bytes + offset);
        uint32_t record_length = read32(bytes + offset + 4);
        const uint8_t *data = bytes + offset + 8;
        if (record_length > length - offset - 8) {
            return -6;
        }
        int result = 0;
        switch (type) {
            case RECORD_SECTION_RUNTIME:
            case RECORD_SECURE_SECTION_RUNTIME:
                result = store_section(out, &phase, data, record_length);
                break;
            case RECORD_FLAGS:
                if (record_length >= 4) {
                    out->flags = read32(data);
                }
                break;
            case RECORD_PHY_SKU:
                if (record_length != 4) {
                    return -7;
                }
                out->phy_config = read32(data);
                out->valid_tx_antennas = (uint8_t)((out->phy_config >> PHY_CONFIG_TX_CHAIN_POSITION) & 0xF);
                out->valid_rx_antennas = (uint8_t)((out->phy_config >> PHY_CONFIG_RX_CHAIN_POSITION) & 0xF);
                break;
            case RECORD_API_CHANGES_SET:
            case RECORD_ENABLED_CAPABILITIES: {
                if (record_length != 8) {
                    return -8;
                }
                uint32_t index = read32(data);
                uint32_t bits = read32(data + 4);
                uint32_t *words = type == RECORD_API_CHANGES_SET ? out->api : out->capabilities;
                if (index < INTEL_WIRELESS_API_WORDS) {
                    words[index] |= bits;
                }
                break;
            }
            case RECORD_SCAN_CHANNELS:
                if (record_length != 4) {
                    return -9;
                }
                out->scan_channels = read32(data);
                break;
            case RECORD_STATIONS:
                if (record_length != 4) {
                    return -10;
                }
                out->stations = read32(data);
                break;
            case RECORD_FIRMWARE_VERSION:
                if (record_length >= 8) {
                    out->major = read32(data);
                    out->minor = read32(data + 4);
                }
                break;
            case RECORD_COMMAND_VERSIONS:
                if (record_length % 4 != 0) {
                    return -11;
                }
                for (uint32_t i = 0; i + 4 <= record_length && out->version_count < INTEL_WIRELESS_MAX_COMMAND_VERSIONS;
                     i += 4) {
                    intel_wireless_command_version_t *v = &out->versions[out->version_count++];
                    v->command = data[i];
                    v->group = data[i + 1];
                    v->command_version = data[i + 2];
                    v->notification_version = data[i + 3];
                }
                break;
            default:
                break;
        }
        if (result != 0) {
            return result;
        }
        offset += 8 + ((record_length + 3u) & ~3u);
    }

    if (out->lmac_count == 0) {
        return -12;
    }
    if (out->umac_count == 0) {
        return -13;
    }
    return 0;
}

static int test_bit(const uint32_t *words, uint32_t count, uint32_t bit) {
    if (bit / 32 >= count) {
        return 0;
    }
    return (words[bit / 32] >> (bit % 32)) & 1u;
}

int intel_wireless_firmware_has_capability(const intel_wireless_firmware_t *firmware, uint32_t bit) {
    return test_bit(firmware->capabilities, INTEL_WIRELESS_CAPABILITY_WORDS, bit);
}

int intel_wireless_firmware_has_api(const intel_wireless_firmware_t *firmware, uint32_t bit) {
    return test_bit(firmware->api, INTEL_WIRELESS_API_WORDS, bit);
}

static const intel_wireless_command_version_t *find_version(const intel_wireless_firmware_t *firmware, uint8_t group,
                                                            uint8_t command) {
    uint8_t wanted = group == 0 ? LONG_GROUP : group;
    for (uint32_t i = 0; i < firmware->version_count; i++) {
        const intel_wireless_command_version_t *v = &firmware->versions[i];
        if (v->command == command && (v->group == wanted || v->group == group)) {
            return v;
        }
    }
    return 0;
}

uint8_t intel_wireless_command_version(const intel_wireless_firmware_t *firmware, uint8_t group, uint8_t command,
                                       uint8_t fallback) {
    const intel_wireless_command_version_t *v = find_version(firmware, group, command);
    return v ? v->command_version : fallback;
}

uint8_t intel_wireless_notification_version(const intel_wireless_firmware_t *firmware, uint8_t group,
                                            uint8_t command, uint8_t fallback) {
    const intel_wireless_command_version_t *v = find_version(firmware, group, command);
    return v ? v->notification_version : fallback;
}

static uint32_t append(char *out, uint32_t at, uint32_t capacity, const char *text) {
    while (*text && at + 1 < capacity) {
        out[at++] = *text++;
    }
    out[at] = 0;
    return at;
}

int intel_wireless_firmware_name(uint32_t hardware_revision, uint32_t rf_id, char *out, uint32_t capacity) {
    if (!out || capacity < 48) {
        return 0;
    }
    uint32_t mac_type = (hardware_revision & 0xFFF0u) >> 4;
    uint32_t mac_step = hardware_revision & 0xFu;
    uint32_t rf_type = (rf_id & 0xFFF000u) >> 12;
    int cdb = (rf_id >> 28) & 1;

    const char *mac;
    if (mac_type == MAC_TYPE_QUZ) {
        mac = "QuZ-a0";
    } else if (mac_type == MAC_TYPE_QU && mac_step == 1) {
        mac = "Qu-b0";
    } else if (mac_type == MAC_TYPE_QU && mac_step == 2) {
        mac = "Qu-c0";
    } else {
        return 0;
    }
    if (rf_type != RF_TYPE_HR1 && rf_type != RF_TYPE_HR2) {
        return 0;
    }

    uint32_t at = append(out, 0, capacity, "iwlwifi-");
    at = append(out, at, capacity, mac);
    at = append(out, at, capacity, cdb ? "-hr4-b0-77.ucode" : "-hr-b0-77.ucode");
    return (int)at;
}
