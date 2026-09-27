#include "usb_storage_protocol.h"

static void put_little_endian32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t read_little_endian32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static uint32_t read_big_endian32(const uint8_t *in) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static void zero(uint8_t *out, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        out[i] = 0;
    }
}

uint32_t usb_storage_build_command_wrapper(uint8_t *out, uint32_t tag, uint32_t transfer_length,
                                           uint8_t direction, uint8_t logical_unit,
                                           const uint8_t *command, uint8_t command_length) {
    if (!out || !command || command_length == 0 || command_length > 16) {
        return 0;
    }
    zero(out, USB_STORAGE_CBW_LENGTH);
    put_little_endian32(out + 0, USB_STORAGE_CBW_SIGNATURE);
    put_little_endian32(out + 4, tag);
    put_little_endian32(out + 8, transfer_length);
    out[12] = (uint8_t)(direction & 0x80u);
    out[13] = (uint8_t)(logical_unit & 0x0Fu);
    out[14] = command_length;
    for (uint8_t i = 0; i < command_length; i++) {
        out[15 + i] = command[i];
    }
    return USB_STORAGE_CBW_LENGTH;
}

int usb_storage_status_wrapper_ok(const uint8_t *csw, uint32_t length, uint32_t tag,
                                  uint32_t *residue_out) {
    if (residue_out) {
        *residue_out = 0;
    }
    if (!csw || length < USB_STORAGE_CSW_LENGTH) {
        return 0;
    }
    if (read_little_endian32(csw + 0) != USB_STORAGE_CSW_SIGNATURE) {
        return 0;
    }
    /* A wrapper carrying somebody else's tag is an answer to a question this
       transfer did not ask, which is worse than no answer at all. */
    if (read_little_endian32(csw + 4) != tag) {
        return 0;
    }
    if (residue_out) {
        *residue_out = read_little_endian32(csw + 8);
    }
    return csw[12] == USB_STORAGE_STATUS_PASSED;
}

uint32_t usb_storage_build_test_unit_ready(uint8_t *out) {
    zero(out, 6);
    out[0] = SCSI_TEST_UNIT_READY;
    return 6;
}

uint32_t usb_storage_build_inquiry(uint8_t *out) {
    zero(out, 6);
    out[0] = SCSI_INQUIRY;
    out[4] = SCSI_INQUIRY_LENGTH;
    return 6;
}

uint32_t usb_storage_build_read_capacity10(uint8_t *out) {
    zero(out, 10);
    out[0] = SCSI_READ_CAPACITY10;
    return 10;
}

static uint32_t build_transfer10(uint8_t *out, uint8_t opcode, uint32_t lba, uint16_t blocks) {
    zero(out, 10);
    out[0] = opcode;
    out[2] = (uint8_t)((lba >> 24) & 0xFFu);
    out[3] = (uint8_t)((lba >> 16) & 0xFFu);
    out[4] = (uint8_t)((lba >> 8) & 0xFFu);
    out[5] = (uint8_t)(lba & 0xFFu);
    out[7] = (uint8_t)((blocks >> 8) & 0xFFu);
    out[8] = (uint8_t)(blocks & 0xFFu);
    return 10;
}

uint32_t usb_storage_build_read10(uint8_t *out, uint32_t lba, uint16_t blocks) {
    return build_transfer10(out, SCSI_READ10, lba, blocks);
}

uint32_t usb_storage_build_write10(uint8_t *out, uint32_t lba, uint16_t blocks) {
    return build_transfer10(out, SCSI_WRITE10, lba, blocks);
}

int usb_storage_parse_read_capacity10(const uint8_t *in, uint32_t length, uint64_t *sectors_out,
                                      uint32_t *sector_bytes_out) {
    if (!in || !sectors_out || !sector_bytes_out || length < SCSI_READ_CAPACITY10_LENGTH) {
        return 0;
    }
    uint32_t last_lba = read_big_endian32(in + 0);
    uint32_t sector_bytes = read_big_endian32(in + 4);

    if (sector_bytes == 0 || (sector_bytes & (sector_bytes - 1)) != 0) {
        return 0;
    }
    if (sector_bytes < 512 || sector_bytes > 4096) {
        return 0;
    }
    if (last_lba == 0 || last_lba == 0xFFFFFFFFu) {
        return 0;
    }

    *sectors_out = (uint64_t)last_lba + 1;
    *sector_bytes_out = sector_bytes;
    return 1;
}

uint32_t usb_storage_transfer_sectors(int superspeed) {
    return superspeed ? USB_STORAGE_SUPERSPEED_TRANSFER_SECTORS
                      : USB_STORAGE_HIGH_SPEED_TRANSFER_SECTORS;
}

uint32_t usb_storage_next_piece(uint64_t buffer_phys, uint32_t remaining) {
    uint64_t to_boundary = USB_STORAGE_PIECE_BYTES - (buffer_phys & (USB_STORAGE_PIECE_BYTES - 1));
    return remaining < to_boundary ? remaining : (uint32_t)to_boundary;
}
