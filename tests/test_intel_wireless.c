#include "check.h"

#include <stdlib.h>

#include "drivers/intel_wireless.h"
#include "drivers/intel_wireless_api.h"
#include "drivers/intel_wireless_registers.h"
#include "drivers/intel_wireless_transport.h"

void fake_physical_memory_reset(void);

/* A model of a 22000-family device and of the firmware running on it - just
   enough of both that the driver's whole path runs: ownership, the context
   block the boot ROM reads, the alive interrupt, the receive ring the driver
   stocks and the device fills, the command ring the driver fills and the
   device drains, and a scan that hears beacons. It is written from the same
   reading of Linux's driver as the code under test, so it cannot catch a
   misreading of the hardware - tools/intel-wireless-test.sh's layout check
   and the laptop do that. What it catches is everything else: ring
   arithmetic, ids, wrap, a reply matched to the wrong command. */

#define REGISTER_SPACE 0x2000

typedef struct {
    uint32_t registers[REGISTER_SPACE / 4];
    uint32_t periphery_write_address;
    uint32_t periphery_read_address;
    uint32_t periphery[64][2];
    uint32_t periphery_count;

    int booted;
    int alive_delivered;
    int never_alive;
    int error_on_command;
    uint8_t refuse_group;
    uint8_t refuse_command;

    uint64_t *free_ring;
    uint32_t *used_ring;
    intel_wireless_rb_status_t *status;
    uint32_t free_taken;
    uint32_t free_announced;
    uint32_t closed;

    intel_wireless_tfd_t *tfds;
    uint32_t command_read;

    uint32_t commands_seen;
    uint8_t command_groups[64];
    uint8_t command_ids[64];
    uint8_t command_versions[64];
    uint8_t mac_node[6];
    uint32_t scan_channels;
    uint16_t scan_flags;
    uint32_t beacons_to_send;
    int context_valid;
    uint32_t widx_not_multiple_of_8;
    uint32_t software_resets;

    uint8_t pending[64][1024];
    uint32_t pending_length[64];
    uint32_t pending_count;
} model_t;

static model_t model;

static const uint8_t lmac_bytes[] = {0x10, 0x11, 0x12, 0x13, 0x14};
static const uint8_t umac_bytes[] = {0x20, 0x21, 0x22};
static const uint8_t paging_bytes[] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36};

static uint32_t *reg(uint32_t offset) {
    return &model.registers[(offset % REGISTER_SPACE) / 4];
}

static uint32_t *periphery(uint32_t address) {
    address &= PERIPHERY_ADDRESS_MASK;
    for (uint32_t i = 0; i < model.periphery_count; i++) {
        if (model.periphery[i][0] == address) {
            return &model.periphery[i][1];
        }
    }
    model.periphery[model.periphery_count][0] = address;
    model.periphery[model.periphery_count][1] = 0;
    return &model.periphery[model.periphery_count++][1];
}

static void queue_packet(uint8_t group, uint8_t command, uint16_t sequence, const void *payload, uint32_t length) {
    REQUIRE(model.pending_count < 64);
    uint8_t *p = model.pending[model.pending_count];
    uint32_t frame = 4 + length;
    p[0] = (uint8_t)frame;
    p[1] = (uint8_t)(frame >> 8);
    p[2] = 0;
    p[3] = 0;
    p[4] = command;
    p[5] = group;
    p[6] = (uint8_t)sequence;
    p[7] = (uint8_t)(sequence >> 8);
    memcpy(p + 8, payload, length);
    model.pending_length[model.pending_count++] = 8 + length;
}

/* The device writes as many waiting packets into one buffer as fit, each
   64-byte aligned, and ends the run with the invalid marker - the shape the
   driver has to take apart. */
static void deliver(void) {
    while (model.pending_count > 0 && model.free_taken != model.free_announced) {
        uint64_t entry = model.free_ring[model.free_taken % INTEL_WIRELESS_RX_RING];
        model.free_taken++;
        uint8_t *buffer = (uint8_t *)(uintptr_t)(entry & ~0xFFFull);
        uint32_t id = (uint32_t)(entry & 0xFFF);
        uint32_t offset = 0;
        uint32_t used = 0;
        while (used < model.pending_count) {
            uint32_t aligned = (model.pending_length[used] + 63) & ~63u;
            if (offset + aligned + 4 > INTEL_WIRELESS_RX_BUFFER) {
                break;
            }
            memcpy(buffer + offset, model.pending[used], model.pending_length[used]);
            offset += aligned;
            used++;
            if (used == 3) {
                break;
            }
        }
        uint32_t invalid = FH_RSCSR_FRAME_INVALID;
        memcpy(buffer + offset, &invalid, 4);
        memmove(model.pending, model.pending[used], (model.pending_count - used) * sizeof(model.pending[0]));
        memmove(model.pending_length, model.pending_length + used, (model.pending_count - used) * sizeof(uint32_t));
        model.pending_count -= used;
        model.used_ring[model.closed % INTEL_WIRELESS_RX_RING] = id;
        model.closed++;
        model.status->closed_rb_number = (uint16_t)(model.closed & 0xFFF);
        *reg(CSR_INT) |= CSR_INT_BIT_FH_RX;
    }
}

static void boot(void) {
    uint64_t address = (uint64_t)*reg(CSR_CTXT_INFO_BA) | ((uint64_t)*reg(CSR_CTXT_INFO_BA + 4) << 32);
    intel_wireless_context_info_t *context = (intel_wireless_context_info_t *)(uintptr_t)address;
    model.context_valid = context && context->size == sizeof(*context) / 4 &&
                          memcmp((void *)(uintptr_t)context->lmac_image[0], lmac_bytes, sizeof(lmac_bytes)) == 0 &&
                          memcmp((void *)(uintptr_t)context->umac_image[0], umac_bytes, sizeof(umac_bytes)) == 0 &&
                          memcmp((void *)(uintptr_t)context->virtual_image[0], paging_bytes, sizeof(paging_bytes)) == 0 &&
                          context->lmac_image[1] == 0 && context->command_queue_size == 2 &&
                          ((context->control_flags >> 4) & 0xF) == 8;
    model.free_ring = (uint64_t *)(uintptr_t)context->free_rbd_address;
    model.used_ring = (uint32_t *)(uintptr_t)context->used_rbd_address;
    model.status = (intel_wireless_rb_status_t *)(uintptr_t)context->status_write_pointer;
    model.tfds = (intel_wireless_tfd_t *)(uintptr_t)context->command_queue_address;
    model.booted = 1;
    if (model.never_alive) {
        return;
    }
    *reg(CSR_INT) |= CSR_INT_BIT_ALIVE;
    intel_wireless_alive_v6_t alive;
    memset(&alive, 0, sizeof(alive));
    alive.status = IWL_ALIVE_STATUS_OK;
    alive.lmac[0].ucode_major = 77;
    alive.umac.umac_major = 77;
    queue_packet(0, UCODE_ALIVE_NTFY, SEQ_RX_FRAME, &alive, sizeof(alive));
}

static void beacon(uint32_t index) {
    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));
    intel_wireless_rx_mpdu_v1_t *descriptor = (intel_wireless_rx_mpdu_v1_t *)packet;
    uint8_t *frame = packet + sizeof(*descriptor);
    char name[16];
    uint32_t network = index % 3;
    snprintf(name, sizeof(name), "net%u", network);
    frame[0] = 0x80;
    memset(frame + 4, 0xFF, 6);
    frame[10] = frame[16] = 0x02;
    frame[21] = (uint8_t)network;
    frame[15] = (uint8_t)network;
    frame[24 + 8] = 100;
    frame[24 + 10] = network == 2 ? 0x01 : 0x11;
    uint32_t at = 36;
    frame[at++] = 0;
    frame[at++] = (uint8_t)strlen(name);
    memcpy(frame + at, name, strlen(name));
    at += (uint32_t)strlen(name);
    frame[at++] = 3;
    frame[at++] = 1;
    frame[at++] = network == 1 ? 36 : 6;
    if (network == 0) {
        static const uint8_t rsn[] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                      1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
        memcpy(frame + at, rsn, sizeof(rsn));
        at += sizeof(rsn);
    }
    descriptor->mpdu_length = (uint16_t)at;
    descriptor->status = IWL_RX_MPDU_STATUS_CRC_OK | IWL_RX_MPDU_STATUS_OVERRUN_OK;
    descriptor->energy_a = (uint8_t)(40 + network * 10);
    descriptor->energy_b = (uint8_t)(45 + network * 10);
    descriptor->channel = network == 1 ? 36 : 6;
    queue_packet(0, REPLY_RX_MPDU_CMD, SEQ_RX_FRAME, packet, sizeof(*descriptor) + at);
}

static void answer(uint8_t group, uint8_t command, uint16_t sequence, const uint8_t *payload, uint32_t length) {
    uint32_t n = model.commands_seen++;
    if (n < 64) {
        model.command_groups[n] = group;
        model.command_ids[n] = command;
    }
    if (model.error_on_command && model.commands_seen == (uint32_t)model.error_on_command) {
        *reg(CSR_INT) |= CSR_INT_BIT_SW_ERR;
        return;
    }
    if (group == model.refuse_group && command == model.refuse_command) {
        return;
    }
    uint8_t empty[4] = {0};
    if (group == REGULATORY_AND_NVM_GROUP && command == NVM_GET_INFO) {
        static intel_wireless_nvm_get_info_response_v4_t response;
        memset(&response, 0, sizeof(response));
        response.mac_sku_flags = 3;
        response.tx_chains = 1;
        response.rx_chains = 3;
        response.lar_enabled = 1;
        response.channel_count = 51;
        for (int i = 0; i < 51; i++) {
            response.channel_profile[i] = (i < 13 || (i >= 14 && i < 47)) ? 0x9 : 0;
        }
        queue_packet(group, command, sequence, &response, sizeof(response));
        return;
    }
    if (group == LONG_GROUP && command == MCC_UPDATE_CMD) {
        intel_wireless_mcc_update_response_v4_t response;
        memset(&response, 0, sizeof(response));
        response.mcc = ('U' << 8) | 'S';
        queue_packet(group, command, sequence, &response, sizeof(response));
        return;
    }
    if (group == LONG_GROUP && command == MAC_CONTEXT_CMD) {
        const intel_wireless_mac_context_t *context = (const intel_wireless_mac_context_t *)payload;
        if (length >= sizeof(*context)) {
            memcpy(model.mac_node, context->node_address, 6);
        }
    }
    queue_packet(group, command, sequence, empty, sizeof(empty));
    if (group == REGULATORY_AND_NVM_GROUP && command == NVM_ACCESS_COMPLETE) {
        queue_packet(0, INIT_COMPLETE_NOTIF, SEQ_RX_FRAME, empty, sizeof(empty));
    }
    if (group == LONG_GROUP && command == SCAN_REQ_UMAC) {
        const intel_wireless_scan_request_v17_t *request = (const intel_wireless_scan_request_v17_t *)payload;
        model.scan_channels = request->channel.count;
        model.scan_flags = request->general.flags;
        for (uint32_t i = 0; i < model.beacons_to_send && model.pending_count < 62; i++) {
            beacon(i);
        }
        intel_wireless_scan_complete_t complete;
        memset(&complete, 0, sizeof(complete));
        complete.uid = request->uid;
        complete.status = 1;
        queue_packet(0, SCAN_COMPLETE_UMAC, SEQ_RX_FRAME, &complete, sizeof(complete));
    }
}

static void drain_commands(uint32_t write) {
    while (model.command_read != write) {
        intel_wireless_tfd_t *tfd = &model.tfds[model.command_read % INTEL_WIRELESS_COMMAND_SLOTS];
        static uint8_t command[4096];
        uint32_t length = 0;
        for (uint32_t b = 0; b < (tfd->buffer_count & 0x1F); b++) {
            memcpy(command + length, (void *)(uintptr_t)tfd->buffers[b].address, tfd->buffers[b].length);
            length += tfd->buffers[b].length;
        }
        const intel_wireless_command_header_t *header = (const intel_wireless_command_header_t *)command;
        uint32_t n = model.commands_seen;
        if (n < 64) {
            model.command_versions[n] = header->version;
        }
        if (header->length + sizeof(*header) == length) {
            answer(header->group, header->command, header->sequence, command + sizeof(*header), header->length);
        }
        model.command_read = (model.command_read + 1) & 0xFF;
    }
}

uint32_t intel_wireless_host_read32(uint32_t offset) {
    if (offset == HBUS_TARG_PRPH_RDAT) {
        return *periphery(model.periphery_read_address);
    }
    if (offset == CSR_HW_REV) {
        return 0x350;
    }
    if (offset == CSR_HW_RF_ID) {
        return 0x0010A100;
    }
    if (offset == 0x388) {
        return 0x00000000;
    }
    if (offset == 0x380) {
        return 0xA4C3F001u;
    }
    if (offset == 0x384) {
        return 0x00007E21u;
    }
    return *reg(offset);
}

void intel_wireless_host_write32(uint32_t offset, uint32_t value) {
    if (offset == CSR_INT || offset == CSR_FH_INT_STATUS) {
        *reg(offset) &= ~value;
        return;
    }
    if (offset == HBUS_TARG_PRPH_WADDR) {
        model.periphery_write_address = value;
        return;
    }
    if (offset == HBUS_TARG_PRPH_RADDR) {
        model.periphery_read_address = value;
        return;
    }
    if (offset == HBUS_TARG_PRPH_WDAT) {
        *periphery(model.periphery_write_address) = value;
        if ((model.periphery_write_address & PERIPHERY_ADDRESS_MASK) == (UREG_CPU_INIT_RUN & PERIPHERY_ADDRESS_MASK) &&
            value == 1) {
            boot();
        }
        return;
    }
    if (offset == CSR_GP_CNTRL) {
        if (value & (CSR_GP_CNTRL_REG_FLAG_INIT_DONE | CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ)) {
            value |= CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY;
        }
        value |= CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW;
        *reg(offset) = value;
        return;
    }
    if (offset == CSR_RESET && (value & CSR_RESET_REG_FLAG_SW_RESET)) {
        model.software_resets++;
        *reg(offset) = value & ~CSR_RESET_REG_FLAG_SW_RESET;
        return;
    }
    if (offset == CSR_RESET && (value & CSR_RESET_REG_FLAG_STOP_MASTER)) {
        *reg(offset) = value | CSR_RESET_REG_FLAG_MASTER_DISABLED;
        return;
    }
    if (offset == RFH_Q0_FRBDCB_WIDX_TRG) {
        if (value % 8 != 0) {
            model.widx_not_multiple_of_8++;
        }
        model.free_announced += (value - model.free_announced) & (INTEL_WIRELESS_RX_RING - 1);
        deliver();
        return;
    }
    if (offset == HBUS_TARG_WRPTR) {
        drain_commands(value & 0xFF);
        deliver();
        return;
    }
    *reg(offset) = value;
}

void intel_wireless_host_delay(uint32_t microseconds) {
    (void)microseconds;
    if (model.booted) {
        deliver();
    }
}

static uint8_t firmware_file[2048];
static uint32_t firmware_length;

static void put32(uint8_t *at, uint32_t value) {
    memcpy(at, &value, 4);
}

static void record(uint32_t type, const uint8_t *data, uint32_t length) {
    put32(firmware_file + firmware_length, type);
    put32(firmware_file + firmware_length + 4, length);
    memcpy(firmware_file + firmware_length + 8, data, length);
    firmware_length += 8 + ((length + 3) & ~3u);
}

static void section(uint32_t offset, const uint8_t *bytes, uint32_t length) {
    uint8_t data[64];
    put32(data, offset);
    memcpy(data + 4, bytes, length);
    record(19, data, 4 + length);
}

static void make_firmware(void) {
    memset(firmware_file, 0, sizeof(firmware_file));
    put32(firmware_file + 4, INTEL_WIRELESS_FIRMWARE_MAGIC);
    memcpy(firmware_file + 8, "model", 5);
    put32(firmware_file + 72, 77);
    firmware_length = 88;
    uint8_t phy[4];
    put32(phy, 0x00330018);
    record(23, phy, 4);
    uint8_t capa[8];
    put32(capa, 0);
    put32(capa + 4, (1u << IWL_UCODE_TLV_CAPA_LAR_SUPPORT) | (1u << IWL_UCODE_TLV_CAPA_UMAC_SCAN) |
                        (1u << IWL_UCODE_TLV_CAPA_DQA_SUPPORT));
    record(30, capa, 8);
    put32(capa, 1);
    put32(capa + 4, 1u << (IWL_UCODE_TLV_CAPA_SOC_LATENCY_SUPPORT - 32));
    record(30, capa, 8);
    section(0x433000, lmac_bytes, sizeof(lmac_bytes));
    section(INTEL_WIRELESS_LMAC_UMAC_SEPARATOR, 0, 0);
    section(0x80433000, umac_bytes, sizeof(umac_bytes));
    section(INTEL_WIRELESS_PAGING_SEPARATOR, 0, 0);
    section(0x1000000, paging_bytes, sizeof(paging_bytes));
    uint8_t versions[] = {0x0D, 0x01, 15, 0, 0x28, 0x01, 5, 0, 0x02, 0x0C, 1, 4};
    record(48, versions, sizeof(versions));
}

static void reset(void) {
    intel_wireless_stop();
    fake_physical_memory_reset();
    memset(&model, 0, sizeof(model));
    make_firmware();
}

TEST(intel_wireless, the_firmware_comes_up_through_the_context_block) {
    reset();
    CHECK_EQ(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length), 0);
    CHECK(model.context_valid);
    CHECK_EQ(intel_wireless_state(), INTEL_WIRELESS_STATE_READY);
    CHECK_EQ(model.widx_not_multiple_of_8, 0u);
    uint8_t address[6];
    REQUIRE(intel_wireless_mac_address(address));
    CHECK_EQ(address[0], 0xA4);
    CHECK_EQ(address[3], 0x01);
    CHECK_EQ(address[4], 0x7E);
    CHECK_EQ(address[5], 0x21);
    CHECK_EQ(memcmp(model.mac_node, address, 6), 0);
}

TEST(intel_wireless, legacy_commands_go_out_in_the_long_group_with_the_firmwares_version) {
    reset();
    REQUIRE(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length) == 0);
    int saw_mac = 0;
    int saw_nvm = 0;
    for (uint32_t i = 0; i < model.commands_seen && i < 64; i++) {
        CHECK_MSG(model.command_groups[i] != 0, "command %02x went out in group 0", model.command_ids[i]);
        if (model.command_groups[i] == LONG_GROUP && model.command_ids[i] == MAC_CONTEXT_CMD) {
            saw_mac = 1;
            CHECK_EQ(model.command_versions[i], 5);
        }
        if (model.command_groups[i] == REGULATORY_AND_NVM_GROUP && model.command_ids[i] == NVM_GET_INFO) {
            saw_nvm = 1;
            CHECK_EQ(model.command_versions[i], 1);
        }
    }
    CHECK(saw_mac);
    CHECK(saw_nvm);
    CHECK_EQ(model.command_groups[0], SYSTEM_GROUP);
    CHECK_EQ(model.command_ids[0], INIT_EXTENDED_CFG_CMD);
}

TEST(intel_wireless, a_scan_hears_beacons_through_a_ring_that_wraps) {
    reset();
    REQUIRE(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length) == 0);
    model.beacons_to_send = 60;
    for (int round = 0; round < 30; round++) {
        CHECK_EQ(intel_wireless_scan(), 0);
    }
    CHECK(model.closed > INTEL_WIRELESS_RX_RING * 2);
    CHECK_EQ(model.widx_not_multiple_of_8, 0u);
    CHECK_EQ(model.scan_channels, 46u);
    CHECK(model.scan_flags & IWL_UMAC_SCAN_GEN_FLAGS_V2_FORCE_PASSIVE);

    intel_wireless_network_t networks[8];
    uint32_t count = intel_wireless_networks(networks, 8);
    REQUIRE(count == 3);
    CHECK_EQ(strcmp(networks[0].network.ssid, "net0"), 0);
    CHECK_EQ(networks[0].signal_dbm, -40);
    CHECK_EQ(networks[0].network.security, IEEE80211_SECURITY_WPA2_PSK);
    CHECK_EQ(networks[0].network.channel, 6);
    CHECK_EQ(strcmp(networks[1].network.ssid, "net1"), 0);
    CHECK_EQ(networks[1].network.channel, 36);
    CHECK_EQ(networks[1].band, PHY_BAND_5);
    CHECK_EQ(networks[1].network.security, IEEE80211_SECURITY_WEP);
    CHECK_EQ(strcmp(networks[2].network.ssid, "net2"), 0);
    CHECK_EQ(networks[2].network.security, IEEE80211_SECURITY_OPEN);
    CHECK(networks[0].seen > 1);
}

TEST(intel_wireless, a_firmware_that_never_says_alive_is_stopped_not_waited_on) {
    reset();
    model.never_alive = 1;
    CHECK(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length) < 0);
    CHECK_EQ(intel_wireless_state(), INTEL_WIRELESS_STATE_FAILED);
    CHECK(model.booted);
    CHECK(model.software_resets >= 3);
    CHECK(*reg(CSR_RESET) & CSR_RESET_REG_FLAG_MASTER_DISABLED);
}

TEST(intel_wireless, a_firmware_error_mid_command_fails_the_command) {
    reset();
    model.error_on_command = 4;
    CHECK_EQ(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length),
             INTEL_WIRELESS_FIRMWARE_ERROR);
    CHECK_EQ(intel_wireless_state(), INTEL_WIRELESS_STATE_FAILED);
}

TEST(intel_wireless, a_command_nobody_answers_times_out) {
    reset();
    model.refuse_group = LONG_GROUP;
    model.refuse_command = BT_CONFIG;
    CHECK_EQ(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length),
             INTEL_WIRELESS_TIMED_OUT);
}

TEST(intel_wireless, a_file_that_is_not_firmware_never_reaches_the_device) {
    reset();
    firmware_file[4] ^= 0xFF;
    CHECK_EQ(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length),
             INTEL_WIRELESS_FIRMWARE_ERROR);
    CHECK(!model.booted);
}
