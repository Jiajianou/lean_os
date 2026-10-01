#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/intel_wireless_firmware.h"

/* The parser against the files Intel actually ships. What is checked is
   what the transport relies on: every section fits one 32 KB entry of the
   context block's DRAM map, there are no more of each kind than the map has
   entries for, and the antenna mask and version table are there. */
static int check(const char *path, const char *expected_name, uint32_t hardware_revision, uint32_t rf_id) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        printf("FAIL %s: cannot open\n", path);
        return 1;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc((size_t)length);
    if (fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        printf("FAIL %s: short read\n", path);
        return 1;
    }
    fclose(file);

    static intel_wireless_firmware_t firmware;
    int result = intel_wireless_firmware_parse(bytes, (uint32_t)length, &firmware);
    int failures = 0;
#define EXPECT(condition)                                     \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("FAIL %s: %s\n", path, #condition);        \
            failures++;                                       \
        }                                                     \
    } while (0)
    EXPECT(result == 0);
    EXPECT(firmware.lmac_count > 0 && firmware.lmac_count <= INTEL_WIRELESS_DRAM_ENTRIES_CHECK);
    EXPECT(firmware.umac_count > 0 && firmware.umac_count <= INTEL_WIRELESS_DRAM_ENTRIES_CHECK);
    EXPECT(firmware.paging_count > 0 && firmware.paging_count <= INTEL_WIRELESS_DRAM_ENTRIES_CHECK);
    for (uint32_t i = 0; i < firmware.lmac_count; i++) {
        EXPECT(firmware.lmac[i].length <= 32768);
    }
    for (uint32_t i = 0; i < firmware.umac_count; i++) {
        EXPECT(firmware.umac[i].length <= 32768);
    }
    for (uint32_t i = 0; i < firmware.paging_count; i++) {
        EXPECT(firmware.paging[i].length <= 32768);
    }
    EXPECT(firmware.valid_tx_antennas == 3);
    EXPECT(firmware.valid_rx_antennas == 3);
    EXPECT(firmware.version_count > 100);
    EXPECT(firmware.api_version == 77);
    EXPECT(firmware.scan_channels >= 50);
    EXPECT(intel_wireless_command_version(&firmware, 0, 0x0D, 0) == 15);
    EXPECT(intel_wireless_notification_version(&firmware, 0, 0x01, 0) == 6);
    EXPECT(intel_wireless_notification_version(&firmware, 0x0C, 0x02, 0) == 4);
    char name[64];
    EXPECT(intel_wireless_firmware_name(hardware_revision, rf_id, name, sizeof(name)) > 0);
    EXPECT(strcmp(name, expected_name) == 0);
    EXPECT(strstr(path, expected_name) != NULL);
    printf("%s %s: %u LMAC, %u UMAC and %u paging sections, %u command versions, \"%s\"\n",
           failures ? "FAIL" : "ok  ", expected_name, firmware.lmac_count, firmware.umac_count,
           firmware.paging_count, firmware.version_count, firmware.human_readable);
    free(bytes);
    return failures;
}

int main(int argc, char **argv) {
    const char *directory = argc > 1 ? argv[1] : "build/firmware";
    char path[512];
    int failures = 0;
    snprintf(path, sizeof(path), "%s/iwlwifi-QuZ-a0-hr-b0-77.ucode", directory);
    failures += check(path, "iwlwifi-QuZ-a0-hr-b0-77.ucode", 0x350, 0x0010A100);
    snprintf(path, sizeof(path), "%s/iwlwifi-Qu-b0-hr-b0-77.ucode", directory);
    failures += check(path, "iwlwifi-Qu-b0-hr-b0-77.ucode", 0x331, 0x0010A100);
    snprintf(path, sizeof(path), "%s/iwlwifi-Qu-c0-hr-b0-77.ucode", directory);
    failures += check(path, "iwlwifi-Qu-c0-hr-b0-77.ucode", 0x332, 0x0010C000);
    printf("%s: Intel's three firmware files for this driver parse as the transport needs them\n",
           failures ? "FAIL" : "PASS");
    return failures != 0;
}
