#include "check.h"

#include <stdlib.h>

#include "drivers/intel_wireless.h"
#include "drivers/intel_wireless_api.h"
#include "drivers/intel_wireless_registers.h"
#include "drivers/intel_wireless_transport.h"
#include "network/wireless_manager.h"

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

    intel_wireless_tfd_t *data_tfds;
    uint16_t *byte_counts;
    uint32_t data_cb_size;
    uint32_t data_read;
    int data_queue_added;
    uint8_t station_address[6];
    uint16_t station_association_id;
    uint32_t station_commands;
    uint32_t key_commands;
    uint16_t last_key_flags;
    uint8_t last_key_offset;
    uint8_t last_key[16];
    uint32_t frames_sent;
    uint16_t frame_subtypes[32];
    uint32_t frame_flags[32];
    uint32_t frame_rates[32];
    uint8_t frame_protected[32];
    uint32_t asserted_on_queue_removal;
    uint8_t last_data[512];
    uint32_t last_data_length;
    uint32_t byte_count_mismatches;
    int refuse_association;
    int silent_access_point;
    int follow_association;
    int dead;
    uint32_t boots;
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
    /* A second boot is the driver loading the firmware again after it died:
       every ring starts over, on both sides. */
    model.dead = 0;
    model.boots++;
    model.command_read = 0;
    model.free_taken = model.free_announced = model.closed = 0;
    model.pending_count = 0;
    model.data_queue_added = 0;
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

#define MODEL_QUEUE 9
#define MODEL_WRITE_POINTER 7
#define MODEL_ACCESS_POINT_BYTE 2

static const uint8_t model_bssid[6] = {0x02, 0, 0, 0, 0, MODEL_ACCESS_POINT_BYTE};

/* A received 802.11 frame as the firmware hands it up, with the descriptor
   in front. `pad` puts two bytes after `split` bytes of the frame, as the
   hardware does after a header that is not a whole number of words. */
static void receive_frame(const uint8_t *frame, uint32_t length, uint32_t status, uint32_t split, int pad) {
    uint8_t packet[600];
    memset(packet, 0, sizeof(packet));
    intel_wireless_rx_mpdu_v1_t *descriptor = (intel_wireless_rx_mpdu_v1_t *)packet;
    uint8_t *out = packet + sizeof(*descriptor);
    uint32_t total = length;
    if (pad) {
        memcpy(out, frame, split);
        memcpy(out + split + 2, frame + split, length - split);
        total += 2;
        descriptor->mac_flags2 = IWL_RX_MPDU_MFLG2_PAD;
    } else {
        memcpy(out, frame, length);
    }
    descriptor->mpdu_length = (uint16_t)total;
    descriptor->status = IWL_RX_MPDU_STATUS_CRC_OK | IWL_RX_MPDU_STATUS_OVERRUN_OK | status;
    descriptor->energy_a = 50;
    descriptor->channel = 6;
    queue_packet(0, REPLY_RX_MPDU_CMD, SEQ_RX_FRAME, packet, sizeof(*descriptor) + total);
}

static void management_reply(uint8_t subtype, const uint8_t *body, uint32_t body_length) {
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    frame[0] = subtype;
    memcpy(frame + 4, model.mac_node, 6);
    memcpy(frame + 10, model_bssid, 6);
    memcpy(frame + 16, model_bssid, 6);
    memcpy(frame + 24, body, body_length);
    receive_frame(frame, 24 + body_length, 0, 0, 0);
}

/* The access point's half of joining, and the firmware's report of each
   frame it sent. */
static void transmitted(const uint8_t *frame, uint32_t length) {
    uint16_t frame_control = (uint16_t)(frame[0] | frame[1] << 8);
    if (model.silent_access_point) {
        return;
    }
    if ((frame_control & 0x0C) == 0 && (frame_control & 0xF0) == 0xB0) {
        uint8_t body[6] = {0, 0, 2, 0, 0, 0};
        management_reply(0xB0, body, sizeof(body));
    } else if ((frame_control & 0x0C) == 0 && (frame_control & 0xF0) == 0x00) {
        uint8_t body[6] = {0x01, 0x04, (uint8_t)(model.refuse_association ? 17 : 0), 0, 0x05, 0xC0};
        management_reply(0x10, body, sizeof(body));
        if (!model.refuse_association) {
            uint8_t data[24 + 8 + 28];
            memset(data, 0, sizeof(data));
            data[0] = 0x08;
            data[1] = 0x02;
            memcpy(data + 4, model.mac_node, 6);
            memcpy(data + 10, model_bssid, 6);
            memcpy(data + 16, model_bssid, 6);
            const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x08, 0x06};
            memcpy(data + 24, snap, 8);
            data[32] = 0x5A;
            receive_frame(data, sizeof(data), 0, 0, 0);
            model.follow_association = 1;
        }
    } else if ((frame_control & 0x0C) == 0x08 && length <= sizeof(model.last_data)) {
        memcpy(model.last_data, frame, length);
        model.last_data_length = length;
    }
}

static void drain_data(uint32_t write) {
    while (model.data_read != write) {
        uint32_t index = model.data_read & ((1u << (model.data_cb_size + 3)) - 1);
        intel_wireless_tfd_t *tfd = &model.data_tfds[index];
        uint8_t command[64];
        uint32_t command_length = 0;
        for (uint32_t b = 0; b < 2 && b < (tfd->buffer_count & 0x1F); b++) {
            memcpy(command + command_length, (void *)(uintptr_t)tfd->buffers[b].address, tfd->buffers[b].length);
            command_length += tfd->buffers[b].length;
        }
        REQUIRE(tfd->buffers[0].length == 20);
        const intel_wireless_short_header_t *header = (const intel_wireless_short_header_t *)command;
        const intel_wireless_tx_command_t *tx = (const intel_wireless_tx_command_t *)(command + 4);
        uint8_t frame[2048];
        uint32_t header_length = (uint32_t)((tx->offload_assist >> TX_CMD_OFFLD_MH_SIZE) & 0x1F) * 2;
        memcpy(frame, command + 24, header_length);
        uint32_t frame_length = header_length;
        if ((tfd->buffer_count & 0x1F) == 3) {
            memcpy(frame + header_length, (void *)(uintptr_t)tfd->buffers[2].address, tfd->buffers[2].length);
            frame_length += tfd->buffers[2].length;
        }
        if (frame_length != tx->length ||
            model.byte_counts[index] != (uint16_t)((tx->length + 3) / 4) || header->command != TX_CMD) {
            model.byte_count_mismatches++;
        }
        uint32_t n = model.frames_sent++;
        if (n < 32) {
            model.frame_subtypes[n] = (uint16_t)(frame[0] & 0xFC);
            model.frame_flags[n] = tx->flags;
            model.frame_rates[n] = tx->rate_n_flags;
            model.frame_protected[n] = (uint8_t)((frame[1] & 0x40) != 0);
        }
        intel_wireless_tx_response_t response;
        memset(&response, 0, sizeof(response));
        response.frame_count = 1;
        response.tx_queue = MODEL_QUEUE;
        response.status = TX_STATUS_SUCCESS;
        queue_packet(LONG_GROUP, TX_CMD, header->sequence, &response, sizeof(response));
        transmitted(frame, frame_length);
        model.data_read = (model.data_read + 1) & 0xFF;
    }
}

static void answer(uint8_t group, uint8_t command, uint16_t sequence, const uint8_t *payload, uint32_t length) {
    if (model.dead) {
        return;
    }
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
    if (group == LONG_GROUP && command == ADD_STA) {
        const intel_wireless_add_station_t *station = (const intel_wireless_add_station_t *)payload;
        REQUIRE(length == sizeof(*station));
        if (station->add_modify == STA_MODE_ADD) {
            memcpy(model.station_address, station->address, 6);
        }
        model.station_association_id = station->association_id;
        model.station_commands++;
        uint32_t status = ADD_STA_SUCCESS;
        queue_packet(group, command, sequence, &status, sizeof(status));
        return;
    }
    if (group == LONG_GROUP && command == ADD_STA_KEY) {
        const intel_wireless_add_station_key_t *key = (const intel_wireless_add_station_key_t *)payload;
        REQUIRE(length == sizeof(*key));
        model.key_commands++;
        model.last_key_flags = key->key_flags;
        model.last_key_offset = key->key_offset;
        memcpy(model.last_key, key->key, 16);
        uint32_t status = ADD_STA_SUCCESS;
        queue_packet(group, command, sequence, &status, sizeof(status));
        return;
    }
    if (group == DATA_PATH_GROUP && command == SCD_QUEUE_CONFIG_CMD) {
        const intel_wireless_queue_config_t *queue = (const intel_wireless_queue_config_t *)payload;
        if (queue->operation == IWL_SCD_QUEUE_ADD) {
            REQUIRE(length == sizeof(*queue));
            CHECK_EQ(queue->station_mask, 1u);
            CHECK_EQ(queue->tid, IWL_MGMT_TID);
            model.data_tfds = (intel_wireless_tfd_t *)(uintptr_t)queue->tfd_queue_address;
            model.byte_counts = (uint16_t *)(uintptr_t)queue->byte_count_address;
            model.data_cb_size = queue->cb_size;
            model.data_read = MODEL_WRITE_POINTER;
            model.data_queue_added = 1;
            intel_wireless_queue_config_response_t response = {MODEL_QUEUE, 0, MODEL_WRITE_POINTER, 0};
            queue_packet(group, command, sequence, &response, sizeof(response));
        } else {
            /* The laptop's firmware asserts on a removal shorter than the
               whole command - its error table said 36 expected, 12 given -
               and answers nothing after. */
            if (length != sizeof(*queue)) {
                model.asserted_on_queue_removal++;
                *reg(CSR_INT) |= CSR_INT_BIT_SW_ERR;
                model.dead = 1;
                return;
            }
            model.data_queue_added = 0;
            queue_packet(group, command, sequence, empty, sizeof(empty));
        }
        return;
    }
    if (group == MAC_CONF_GROUP && command == SESSION_PROTECTION_CMD) {
        queue_packet(group, command, sequence, empty, sizeof(empty));
        const intel_wireless_session_protection_t *session = (const intel_wireless_session_protection_t *)payload;
        if (session->action == FW_CTXT_ACTION_ADD) {
            intel_wireless_session_protection_notification_t notification = {0, 1, 1, 0};
            queue_packet(MAC_CONF_GROUP, SESSION_PROTECTION_NOTIF, SEQ_RX_FRAME, &notification,
                         sizeof(notification));
        }
        return;
    }
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
        if ((value >> 16) == 0) {
            drain_commands(value & 0xFF);
        } else if ((value >> 16) == MODEL_QUEUE && model.data_queue_added) {
            drain_data(value & 0xFF);
        }
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
    uint8_t versions[] = {0x0D, 0x01, 15, 0, 0x28, 0x01, 5, 0, 0x02, 0x0C, 1, 4, 0x08, 0x01, 4, 0,
                          0x18, 0x01, 12, 2, 0x17, 0x01, 3, 0, 0x1C, 0x01, 9, 7, 0x2B, 0x01, 2, 1,
                          0x17, 0x05, 3, 2, 0x08, 0x05, 2, 0, 0x0F, 0x05, 4, 0, 0x05, 0x03, 1, 0};
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

void fake_wireless_glue_reset(int keep_storage);
const uint8_t *fake_wireless_delivered(uint32_t index, uint32_t *length);
uint32_t fake_wireless_delivered_count(void);

static uint64_t clock_ms;

static void join_open_network(void) {
    reset();
    REQUIRE(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length) == 0);
    fake_wireless_glue_reset(0);
    wireless_manager_reset(intel_wireless_backend());
    model.beacons_to_send = 6;
    clock_ms = 1000;
    wireless_step(clock_ms);
    os_wireless_connect_t request;
    memset(&request, 0, sizeof(request));
    memcpy(request.ssid, "net2", 4);
    request.ssid_length = 4;
    request.remember = 1;
    REQUIRE(wireless_request_connect(&request) == 0);
    clock_ms += 20;
    wireless_step(clock_ms);
}

static int command_index(uint32_t from, uint8_t group, uint8_t command) {
    for (uint32_t i = from; i < model.commands_seen && i < 64; i++) {
        if (model.command_groups[i] == group && model.command_ids[i] == command) {
            return (int)i;
        }
    }
    return -1;
}

TEST(intel_wireless, an_open_network_is_joined_through_the_firmware_in_linuxs_order) {
    join_open_network();
    os_wireless_status_t status;
    wireless_status(&status);
    CHECK_EQ(status.state, WIRELESS_STATE_CONNECTED);

    int phy = command_index(0, LONG_GROUP, PHY_CONTEXT_CMD);
    int rlc = command_index(0, DATA_PATH_GROUP, RLC_CONFIG_CMD);
    int binding = command_index(0, LONG_GROUP, BINDING_CONTEXT_CMD);
    int station = command_index(0, LONG_GROUP, ADD_STA);
    int queue = command_index(0, DATA_PATH_GROUP, SCD_QUEUE_CONFIG_CMD);
    int session = command_index(0, MAC_CONF_GROUP, SESSION_PROTECTION_CMD);
    int rates = command_index(0, DATA_PATH_GROUP, TLC_MNG_CONFIG_CMD);
    REQUIRE(phy >= 0 && rlc >= 0 && binding >= 0 && station >= 0 && queue >= 0 && session >= 0 && rates >= 0);
    int bssid = command_index((uint32_t)binding, LONG_GROUP, MAC_CONTEXT_CMD);
    CHECK(bssid > binding && bssid < station);
    CHECK(phy < rlc && rlc < binding && binding < station && station < queue && queue < session &&
          session < rates);
    CHECK(command_index((uint32_t)station + 1, LONG_GROUP, ADD_STA) > session);
    CHECK_EQ(model.command_versions[phy], 4);
    CHECK_EQ(model.command_versions[station], 12);
    CHECK_EQ(model.command_versions[queue], 3);

    CHECK_EQ(memcmp(model.station_address, model_bssid, 6), 0);
    CHECK_EQ(model.station_association_id, 5);
    REQUIRE(model.frames_sent >= 2);
    CHECK_EQ(model.frame_subtypes[0], 0xB0);
    CHECK_EQ(model.frame_subtypes[1], 0x00);
    uint32_t wanted = IWL_TX_FLAGS_CMD_RATE | IWL_TX_FLAGS_ENCRYPT_DIS | IWL_TX_FLAGS_HIGH_PRI;
    CHECK_EQ(model.frame_flags[0] & wanted, wanted);
    CHECK_EQ(model.frame_rates[0], (2u << RATE_MCS_ANT_POSITION) | RATE_MCS_MOD_TYPE_CCK);
    CHECK_EQ(model.byte_count_mismatches, 0u);
    CHECK_EQ(intel_wireless_data_in_flight(), 0u);
}

static uint32_t arp_frame(uint8_t *frame) {
    memset(frame, 0, 42);
    memset(frame, 0xFF, 6);
    memcpy(frame + 6, model.mac_node, 6);
    frame[12] = 0x08;
    frame[13] = 0x06;
    for (int i = 0; i < 28; i++) {
        frame[14 + i] = (uint8_t)(0x40 + i);
    }
    return 42;
}

static uint32_t data_from_network(uint8_t *frame, int qos, int protect, uint64_t pn) {
    uint32_t at = 0;
    frame[0] = (uint8_t)(qos ? 0x88 : 0x08);
    frame[1] = (uint8_t)(0x02 | (protect ? 0x40 : 0));
    frame[2] = frame[3] = 0;
    memcpy(frame + 4, model.mac_node, 6);
    memcpy(frame + 10, model_bssid, 6);
    static const uint8_t source[6] = {0x02, 0x99, 0, 0, 0, 1};
    memcpy(frame + 16, source, 6);
    frame[22] = frame[23] = 0;
    at = 24;
    if (qos) {
        frame[24] = frame[25] = 0;
        at = 26;
    }
    if (protect) {
        uint8_t ccmp[8] = {(uint8_t)pn, (uint8_t)(pn >> 8), 0, 0x20, (uint8_t)(pn >> 16), (uint8_t)(pn >> 24), 0, 0};
        memcpy(frame + at, ccmp, 8);
        at += 8;
    }
    static const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x08, 0x06};
    memcpy(frame + at, snap, 8);
    at += 8;
    for (int i = 0; i < 28; i++) {
        frame[at++] = (uint8_t)(0x80 + i);
    }
    return at;
}

TEST(intel_wireless, ip_crosses_the_radio_both_ways) {
    join_open_network();
    uint8_t ethernet[64];
    uint32_t length = arp_frame(ethernet);
    REQUIRE(wireless_link_send(ethernet, (uint16_t)length) == 0);
    clock_ms += 20;
    wireless_step(clock_ms);
    REQUIRE(model.last_data_length == 24 + 8 + 28);
    CHECK_EQ(model.last_data[0], 0x08);
    CHECK_EQ(model.last_data[1], 0x01);
    CHECK_EQ(memcmp(model.last_data + 4, model_bssid, 6), 0);
    CHECK_EQ(memcmp(model.last_data + 10, model.mac_node, 6), 0);
    CHECK_EQ(model.last_data[24], 0xAA);
    CHECK_EQ(model.last_data[31], 0x06);
    CHECK_EQ(memcmp(model.last_data + 32, ethernet + 14, 28), 0);
    uint32_t last = model.frames_sent - 1;
    CHECK(model.frame_flags[last] & IWL_TX_FLAGS_ENCRYPT_DIS);
    CHECK(!(model.frame_flags[last] & IWL_TX_FLAGS_CMD_RATE));
    CHECK(!model.frame_protected[last]);

    uint8_t frame[128];
    uint32_t frame_length = data_from_network(frame, 1, 0, 0);
    receive_frame(frame, frame_length, 0, 26, 1);
    deliver();
    clock_ms += 20;
    wireless_step(clock_ms);
    REQUIRE(fake_wireless_delivered_count() == 2);
    uint32_t delivered_length = 0;
    const uint8_t *early = fake_wireless_delivered(0, &delivered_length);
    CHECK_EQ(early[14], 0x5A);
    const uint8_t *delivered = fake_wireless_delivered(1, &delivered_length);
    REQUIRE(delivered_length == 14 + 28);
    CHECK_EQ(memcmp(delivered, model.mac_node, 6), 0);
    CHECK_EQ(delivered[6], 0x02);
    CHECK_EQ(delivered[7], 0x99);
    CHECK_EQ(delivered[12], 0x08);
    CHECK_EQ(delivered[13], 0x06);
    CHECK_EQ(delivered[14], 0x80);
    CHECK_EQ(delivered[14 + 27], 0x80 + 27);
}

TEST(intel_wireless, keys_go_to_the_firmware_and_protected_frames_are_checked) {
    join_open_network();
    const wireless_backend_t *backend = intel_wireless_backend();
    uint8_t key[16];
    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)(i * 7);
    }
    REQUIRE(backend->install_key(0, key, 16, 0) == 0);
    CHECK_EQ(model.last_key_flags, STA_KEY_FLG_CCM | STA_KEY_FLG_WEP_KEY_MAP);
    CHECK_EQ(model.last_key_offset, 0);
    CHECK_EQ(memcmp(model.last_key, key, 16), 0);
    REQUIRE(backend->install_key(1, key, 16, 1) == 0);
    CHECK_EQ(model.last_key_flags, STA_KEY_FLG_CCM | STA_KEY_FLG_WEP_KEY_MAP | (1u << 8) | STA_KEY_MULTICAST);
    CHECK_EQ(model.last_key_offset, 2);

    uint8_t ethernet[64];
    uint32_t length = arp_frame(ethernet);
    REQUIRE(wireless_link_send(ethernet, (uint16_t)length) == 0);
    clock_ms += 20;
    wireless_step(clock_ms);
    uint32_t last = model.frames_sent - 1;
    CHECK(!(model.frame_flags[last] & IWL_TX_FLAGS_ENCRYPT_DIS));
    CHECK_MSG(model.frame_protected[last], "a frame the radio is to encrypt must say it is protected");
    CHECK_EQ(model.last_data[1] & 0x40, 0x40);
    CHECK_EQ(model.last_data_length, 24u + 8u + 28u);

    uint32_t ok = IWL_RX_MPDU_STATUS_SEC_CCM | IWL_RX_MPDU_STATUS_MIC_OK;
    uint8_t frame[128];
    uint32_t frame_length = data_from_network(frame, 0, 1, 5);
    receive_frame(frame, frame_length, ok, 0, 0);
    receive_frame(frame, frame_length, ok, 0, 0);
    frame_length = data_from_network(frame, 0, 1, 6);
    receive_frame(frame, frame_length, IWL_RX_MPDU_STATUS_SEC_CCM, 0, 0);
    frame_length = data_from_network(frame, 0, 0, 0);
    receive_frame(frame, frame_length, 0, 0, 0);
    frame_length = data_from_network(frame, 1, 1, 7);
    receive_frame(frame, frame_length, ok, 34, 1);
    deliver();
    clock_ms += 20;
    wireless_step(clock_ms);
    CHECK_EQ(fake_wireless_delivered_count(), 3u);
    uint32_t delivered_length = 0;
    const uint8_t *delivered = fake_wireless_delivered(2, &delivered_length);
    REQUIRE(delivered != 0);
    CHECK_EQ(delivered_length, 14u + 28u);
    CHECK_EQ(delivered[14], 0x80);
}

TEST(intel_wireless, a_refused_association_is_reported_and_undone) {
    reset();
    REQUIRE(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length) == 0);
    model.refuse_association = 1;
    fake_wireless_glue_reset(0);
    wireless_manager_reset(intel_wireless_backend());
    model.beacons_to_send = 6;
    wireless_step(1000);
    os_wireless_connect_t request;
    memset(&request, 0, sizeof(request));
    memcpy(request.ssid, "net2", 4);
    request.ssid_length = 4;
    REQUIRE(wireless_request_connect(&request) == 0);
    wireless_step(1020);
    os_wireless_status_t status;
    wireless_status(&status);
    CHECK_EQ(status.state, WIRELESS_STATE_FAILED);
    CHECK_EQ(status.error, WIRELESS_ERROR_REJECTED);
    CHECK(!model.data_queue_added);
    int removed = command_index(0, LONG_GROUP, REMOVE_STA);
    CHECK(removed >= 0);
    CHECK(command_index((uint32_t)removed, LONG_GROUP, PHY_CONTEXT_CMD) > removed);
}

TEST(intel_wireless, an_access_point_that_never_answers_times_out) {
    reset();
    REQUIRE(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length) == 0);
    model.silent_access_point = 1;
    fake_wireless_glue_reset(0);
    wireless_manager_reset(intel_wireless_backend());
    model.beacons_to_send = 6;
    wireless_step(1000);
    os_wireless_connect_t request;
    memset(&request, 0, sizeof(request));
    memcpy(request.ssid, "net2", 4);
    request.ssid_length = 4;
    REQUIRE(wireless_request_connect(&request) == 0);
    wireless_step(1020);
    os_wireless_status_t status;
    wireless_status(&status);
    CHECK_EQ(status.state, WIRELESS_STATE_FAILED);
    CHECK_EQ(status.error, WIRELESS_ERROR_TIMED_OUT);
    CHECK_EQ(model.frames_sent, 3u);
}

/* The laptop's run: a network joined, then left - and the leave's queue
   removal, sent short, killed the firmware, so every join after it waited on
   commands nothing would answer. */
TEST(intel_wireless, leaving_a_network_takes_its_queue_apart_without_killing_the_firmware) {
    join_open_network();
    REQUIRE(model.data_queue_added);
    REQUIRE(wireless_request_disconnect() == 0);
    clock_ms += 20;
    wireless_step(clock_ms);
    CHECK_EQ(model.asserted_on_queue_removal, 0u);
    CHECK(!model.dead);
    CHECK(!model.data_queue_added);
    os_wireless_status_t status;
    wireless_status(&status);
    CHECK_EQ(status.state, WIRELESS_STATE_IDLE);
    CHECK_EQ(intel_wireless_poll(), INTEL_WIRELESS_OK);

    int removed = command_index(0, LONG_GROUP, REMOVE_STA);
    int queue = command_index((uint32_t)command_index(0, DATA_PATH_GROUP, SCD_QUEUE_CONFIG_CMD) + 1,
                              DATA_PATH_GROUP, SCD_QUEUE_CONFIG_CMD);
    CHECK(queue >= 0 && removed > queue);
    CHECK(command_index((uint32_t)removed, LONG_GROUP, PHY_CONTEXT_CMD) > removed);
}

TEST(intel_wireless, a_firmware_that_asserted_is_reported_by_every_poll_until_it_is_loaded_again) {
    reset();
    REQUIRE(intel_wireless_bring_up((volatile uint8_t *)0, 0xA0F0, firmware_file, firmware_length) == 0);
    CHECK_EQ(intel_wireless_poll(), INTEL_WIRELESS_OK);
    *reg(CSR_INT) |= CSR_INT_BIT_SW_ERR;
    model.dead = 1;
    int first = INTEL_WIRELESS_OK;
    for (int i = 0; i < 64 && first == INTEL_WIRELESS_OK; i++) {
        first = intel_wireless_poll();
    }
    CHECK_EQ(first, INTEL_WIRELESS_FIRMWARE_ERROR);
    CHECK_EQ(*reg(CSR_INT) & CSR_INT_BIT_SW_ERR, 0u);
    for (int i = 0; i < 64; i++) {
        CHECK_EQ(intel_wireless_poll(), INTEL_WIRELESS_FIRMWARE_ERROR);
    }
    CHECK_EQ(intel_wireless_scan(), INTEL_WIRELESS_FIRMWARE_ERROR);

    CHECK_EQ(intel_wireless_restart(), INTEL_WIRELESS_OK);
    CHECK_EQ(model.boots, 2u);
    CHECK_EQ(intel_wireless_state(), INTEL_WIRELESS_STATE_READY);
    CHECK_EQ(intel_wireless_poll(), INTEL_WIRELESS_OK);
    model.beacons_to_send = 6;
    CHECK_EQ(intel_wireless_scan(), INTEL_WIRELESS_OK);
}

/* After a restart the manager starts over, and a remembered network is
   joined again by itself - the person does not have to notice the firmware
   died. Nothing from the dead firmware's join is taken apart: those
   commands would be about a PHY and a station the new one never heard of. */
TEST(intel_wireless, a_firmware_that_dies_joined_is_reloaded_and_the_network_rejoined) {
    join_open_network();
    os_wireless_status_t status;
    wireless_status(&status);
    REQUIRE(status.state == WIRELESS_STATE_CONNECTED);

    *reg(CSR_INT) |= CSR_INT_BIT_SW_ERR;
    model.dead = 1;
    for (int i = 0; i < 64 && !wireless_manager_failed(); i++) {
        clock_ms += 20;
        wireless_step(clock_ms);
    }
    REQUIRE(wireless_manager_failed());
    wireless_status(&status);
    CHECK_EQ(status.error, WIRELESS_ERROR_DEVICE);

    REQUIRE(intel_wireless_restart() == INTEL_WIRELESS_OK);
    uint32_t commands_before = model.commands_seen;
    fake_wireless_glue_reset(1);
    wireless_manager_reset(intel_wireless_backend());
    model.beacons_to_send = 6;
    clock_ms += 20;
    wireless_step(clock_ms);
    clock_ms += 20;
    wireless_step(clock_ms);
    wireless_status(&status);
    CHECK_EQ(status.state, WIRELESS_STATE_CONNECTED);
    CHECK_EQ(memcmp(status.ssid, "net2", 4), 0);
    CHECK_EQ(command_index(commands_before, LONG_GROUP, REMOVE_STA), -1);
    CHECK(command_index(commands_before, LONG_GROUP, PHY_CONTEXT_CMD) >= 0);
}
