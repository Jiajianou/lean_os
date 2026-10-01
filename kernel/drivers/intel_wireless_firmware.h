#pragma once

#include <stdint.h>

/* The firmware file Intel publishes for its wireless cards, as linux-firmware
   ships it: a header, then type-length-value records. This reads the records
   a device of the 22000 family (the AX201 in an X1 Carbon Gen 9) needs to be
   started and talked to, and nothing else - the debug records, which are most
   of the file's record count, are skipped. */

#define INTEL_WIRELESS_FIRMWARE_MAGIC 0x0A4C5749u

#define INTEL_WIRELESS_MAX_SECTIONS        64
#define INTEL_WIRELESS_MAX_COMMAND_VERSIONS 256
#define INTEL_WIRELESS_CAPABILITY_WORDS    8
#define INTEL_WIRELESS_API_WORDS           8

#define INTEL_WIRELESS_LMAC_UMAC_SEPARATOR 0xFFFFCCCCu
#define INTEL_WIRELESS_PAGING_SEPARATOR    0xAAAABBBBu

#define INTEL_WIRELESS_COMMAND_VERSION_UNKNOWN 0xFF

typedef struct {
    uint32_t device_offset;
    const uint8_t *data;
    uint32_t length;
} intel_wireless_section_t;

typedef struct {
    uint8_t command;
    uint8_t group;
    uint8_t command_version;
    uint8_t notification_version;
} intel_wireless_command_version_t;

typedef struct {
    char human_readable[65];
    uint32_t api_version;
    uint32_t major;
    uint32_t minor;

    intel_wireless_section_t lmac[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t lmac_count;
    intel_wireless_section_t umac[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t umac_count;
    intel_wireless_section_t paging[INTEL_WIRELESS_MAX_SECTIONS];
    uint32_t paging_count;

    uint32_t api[INTEL_WIRELESS_API_WORDS];
    uint32_t capabilities[INTEL_WIRELESS_CAPABILITY_WORDS];
    uint32_t flags;
    uint32_t phy_config;
    uint8_t valid_tx_antennas;
    uint8_t valid_rx_antennas;
    uint32_t scan_channels;
    uint32_t stations;

    intel_wireless_command_version_t versions[INTEL_WIRELESS_MAX_COMMAND_VERSIONS];
    uint32_t version_count;
} intel_wireless_firmware_t;

/* 0 on success, or a negative number saying which check refused the file -
   the boot log prints it, and a truncated download is a different fix from a
   file for the wrong device. The sections point into `bytes`, which must
   outlive `out`. */
int intel_wireless_firmware_parse(const uint8_t *bytes, uint32_t length, intel_wireless_firmware_t *out);

int intel_wireless_firmware_has_capability(const intel_wireless_firmware_t *firmware, uint32_t bit);
int intel_wireless_firmware_has_api(const intel_wireless_firmware_t *firmware, uint32_t bit);

/* The version of a command this firmware expects, or `fallback` when it does
   not say - which is what Linux's lookup does, and what lets one driver talk
   to several releases of the same firmware. Legacy commands are listed under
   group 1 (the long-header group); this takes group 0 and looks there too. */
uint8_t intel_wireless_command_version(const intel_wireless_firmware_t *firmware, uint8_t group,
                                       uint8_t command, uint8_t fallback);
uint8_t intel_wireless_notification_version(const intel_wireless_firmware_t *firmware, uint8_t group,
                                            uint8_t command, uint8_t fallback);

/* "iwlwifi-QuZ-a0-hr-b0-77.ucode" from the hardware revision and RF id
   registers, the way Linux names the file; 0 when the device is not one this
   driver knows. `out` holds at least 48 bytes. */
int intel_wireless_firmware_name(uint32_t hardware_revision, uint32_t rf_id, char *out, uint32_t capacity);
