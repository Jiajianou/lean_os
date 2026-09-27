#pragma once

#include <stdint.h>

#define USB_STORAGE_CBW_LENGTH 31
#define USB_STORAGE_CSW_LENGTH 13

#define USB_STORAGE_CBW_SIGNATURE 0x43425355u
#define USB_STORAGE_CSW_SIGNATURE 0x53425355u

#define USB_STORAGE_DIRECTION_IN  0x80
#define USB_STORAGE_DIRECTION_OUT 0x00

#define USB_STORAGE_STATUS_PASSED      0
#define USB_STORAGE_STATUS_FAILED      1
#define USB_STORAGE_STATUS_PHASE_ERROR 2

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

/* One command's worth of data. A SuperSpeed stick is given a megabyte, which
   is what Linux gives one; anything slower gets Linux's 240 sectors, because
   older devices are known to fail larger requests. Flash erases in blocks far
   larger than the four kilobytes this driver asked for until M191, so a small
   write costs nearly what a big one does - 36 seconds a megabyte on the first
   laptop it met. */
#define USB_STORAGE_SUPERSPEED_TRANSFER_SECTORS 2048
#define USB_STORAGE_HIGH_SPEED_TRANSFER_SECTORS 240

/* A transfer TRB's buffer may not cross a 64 KiB boundary, so the data phase
   is issued as a run of transfers of at most this size. */
#define USB_STORAGE_PIECE_BYTES 65536u

#define SCSI_INQUIRY_LENGTH        36
#define SCSI_READ_CAPACITY10_LENGTH 8

uint32_t usb_storage_build_command_wrapper(uint8_t *out, uint32_t tag, uint32_t transfer_length,
                                           uint8_t direction, uint8_t logical_unit,
                                           const uint8_t *command, uint8_t command_length);

int usb_storage_status_wrapper_ok(const uint8_t *csw, uint32_t length, uint32_t tag,
                                  uint32_t *residue_out);

uint32_t usb_storage_build_test_unit_ready(uint8_t *out);

uint32_t usb_storage_build_inquiry(uint8_t *out);

uint32_t usb_storage_build_read_capacity10(uint8_t *out);

uint32_t usb_storage_build_read10(uint8_t *out, uint32_t lba, uint16_t blocks);

uint32_t usb_storage_build_write10(uint8_t *out, uint32_t lba, uint16_t blocks);

int usb_storage_parse_read_capacity10(const uint8_t *in, uint32_t length, uint64_t *sectors_out,
                                      uint32_t *sector_bytes_out);

uint32_t usb_storage_transfer_sectors(int superspeed);

uint32_t usb_storage_next_piece(uint64_t buffer_phys, uint32_t remaining);
