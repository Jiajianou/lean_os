#include "check.h"

#include "drivers/usb_storage_protocol.h"

#include <string.h>

TEST(usb_storage, a_command_wrapper_is_the_shape_the_specification_draws) {
    uint8_t command[10];
    uint32_t command_length = usb_storage_build_read10(command, 0x12345678u, 8);
    CHECK_EQ(command_length, 10u);

    uint8_t cbw[USB_STORAGE_CBW_LENGTH];
    memset(cbw, 0xAA, sizeof(cbw));
    CHECK_EQ(usb_storage_build_command_wrapper(cbw, 0xDEADBEEFu, 4096, USB_STORAGE_DIRECTION_IN, 0,
                                               command, (uint8_t)command_length),
             (uint32_t)USB_STORAGE_CBW_LENGTH);

    CHECK_EQ(cbw[0], 'U');
    CHECK_EQ(cbw[1], 'S');
    CHECK_EQ(cbw[2], 'B');
    CHECK_EQ(cbw[3], 'C');
    CHECK_EQ(cbw[4], 0xEF);
    CHECK_EQ(cbw[7], 0xDE);
    CHECK_EQ(cbw[8], 0x00);
    CHECK_EQ(cbw[9], 0x10);
    CHECK_EQ(cbw[12], 0x80);
    CHECK_EQ(cbw[13], 0x00);
    CHECK_EQ(cbw[14], 10);
    CHECK_EQ(cbw[15], SCSI_READ10);
    CHECK_EQ(cbw[30], 0x00);
}

TEST(usb_storage, a_command_longer_than_the_block_is_refused) {
    uint8_t cbw[USB_STORAGE_CBW_LENGTH];
    uint8_t command[20];
    memset(command, 0, sizeof(command));
    CHECK_EQ(usb_storage_build_command_wrapper(cbw, 1, 0, USB_STORAGE_DIRECTION_OUT, 0, command, 17), 0u);
    CHECK_EQ(usb_storage_build_command_wrapper(cbw, 1, 0, USB_STORAGE_DIRECTION_OUT, 0, command, 0), 0u);
}

static void build_status(uint8_t *csw, uint32_t tag, uint32_t residue, uint8_t status) {
    csw[0] = 'U'; csw[1] = 'S'; csw[2] = 'B'; csw[3] = 'S';
    csw[4] = (uint8_t)(tag & 0xFF);
    csw[5] = (uint8_t)((tag >> 8) & 0xFF);
    csw[6] = (uint8_t)((tag >> 16) & 0xFF);
    csw[7] = (uint8_t)((tag >> 24) & 0xFF);
    csw[8] = (uint8_t)(residue & 0xFF);
    csw[9] = (uint8_t)((residue >> 8) & 0xFF);
    csw[10] = (uint8_t)((residue >> 16) & 0xFF);
    csw[11] = (uint8_t)((residue >> 24) & 0xFF);
    csw[12] = status;
}

TEST(usb_storage, a_passing_status_wrapper_is_accepted_with_its_residue) {
    uint8_t csw[USB_STORAGE_CSW_LENGTH];
    build_status(csw, 0x11223344u, 512, USB_STORAGE_STATUS_PASSED);

    uint32_t residue = 0;
    CHECK_EQ(usb_storage_status_wrapper_ok(csw, sizeof(csw), 0x11223344u, &residue), 1);
    CHECK_EQ(residue, 512u);
}

TEST(usb_storage, a_status_wrapper_answering_a_different_tag_is_refused) {
    uint8_t csw[USB_STORAGE_CSW_LENGTH];
    build_status(csw, 0x11223344u, 0, USB_STORAGE_STATUS_PASSED);
    CHECK_EQ(usb_storage_status_wrapper_ok(csw, sizeof(csw), 0x11223345u, 0), 0);
}

TEST(usb_storage, a_failed_or_malformed_status_wrapper_is_refused) {
    uint8_t csw[USB_STORAGE_CSW_LENGTH];

    build_status(csw, 7, 0, USB_STORAGE_STATUS_FAILED);
    CHECK_EQ(usb_storage_status_wrapper_ok(csw, sizeof(csw), 7, 0), 0);

    build_status(csw, 7, 0, USB_STORAGE_STATUS_PHASE_ERROR);
    CHECK_EQ(usb_storage_status_wrapper_ok(csw, sizeof(csw), 7, 0), 0);

    build_status(csw, 7, 0, USB_STORAGE_STATUS_PASSED);
    csw[0] = 'X';
    CHECK_EQ(usb_storage_status_wrapper_ok(csw, sizeof(csw), 7, 0), 0);

    build_status(csw, 7, 0, USB_STORAGE_STATUS_PASSED);
    CHECK_EQ(usb_storage_status_wrapper_ok(csw, USB_STORAGE_CSW_LENGTH - 1, 7, 0), 0);
    CHECK_EQ(usb_storage_status_wrapper_ok(0, sizeof(csw), 7, 0), 0);
}

TEST(usb_storage, the_transfer_commands_carry_their_block_address_big_endian) {
    uint8_t command[10];

    usb_storage_build_read10(command, 0x00ABCDEFu, 0x0102);
    CHECK_EQ(command[0], SCSI_READ10);
    CHECK_EQ(command[2], 0x00);
    CHECK_EQ(command[3], 0xAB);
    CHECK_EQ(command[4], 0xCD);
    CHECK_EQ(command[5], 0xEF);
    CHECK_EQ(command[7], 0x01);
    CHECK_EQ(command[8], 0x02);

    usb_storage_build_write10(command, 1, 1);
    CHECK_EQ(command[0], SCSI_WRITE10);
    CHECK_EQ(command[5], 1);
    CHECK_EQ(command[8], 1);
}

TEST(usb_storage, the_small_commands_are_what_they_should_be) {
    uint8_t command[10];
    CHECK_EQ(usb_storage_build_test_unit_ready(command), 6u);
    CHECK_EQ(command[0], SCSI_TEST_UNIT_READY);

    CHECK_EQ(usb_storage_build_inquiry(command), 6u);
    CHECK_EQ(command[0], SCSI_INQUIRY);
    CHECK_EQ(command[4], SCSI_INQUIRY_LENGTH);

    CHECK_EQ(usb_storage_build_read_capacity10(command), 10u);
    CHECK_EQ(command[0], SCSI_READ_CAPACITY10);
}

TEST(usb_storage, a_capacity_answer_is_the_last_block_not_the_count) {
    const uint8_t answer[8] = {0x00, 0x3B, 0xFF, 0xFF, 0x00, 0x00, 0x02, 0x00};
    uint64_t sectors = 0;
    uint32_t sector_bytes = 0;
    CHECK_EQ(usb_storage_parse_read_capacity10(answer, sizeof(answer), &sectors, &sector_bytes), 1);
    CHECK_EQ(sectors, 0x003BFFFFull + 1);
    CHECK_EQ(sector_bytes, 512u);
}

TEST(usb_storage, a_capacity_answer_that_cannot_be_true_is_refused) {
    uint64_t sectors = 0;
    uint32_t sector_bytes = 0;

    const uint8_t zero_size[8] = {0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK_EQ(usb_storage_parse_read_capacity10(zero_size, 8, &sectors, &sector_bytes), 0);

    const uint8_t odd_size[8] = {0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0xFF};
    CHECK_EQ(usb_storage_parse_read_capacity10(odd_size, 8, &sectors, &sector_bytes), 0);

    const uint8_t huge_size[8] = {0x00, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00};
    CHECK_EQ(usb_storage_parse_read_capacity10(huge_size, 8, &sectors, &sector_bytes), 0);

    const uint8_t no_media[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x02, 0x00};
    CHECK_EQ(usb_storage_parse_read_capacity10(no_media, 8, &sectors, &sector_bytes), 0);

    const uint8_t all_zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_EQ(usb_storage_parse_read_capacity10(all_zero, 8, &sectors, &sector_bytes), 0);

    CHECK_EQ(usb_storage_parse_read_capacity10(zero_size, 7, &sectors, &sector_bytes), 0);
}

TEST(usb_storage, a_superspeed_stick_is_given_a_megabyte_a_command) {
    CHECK_EQ(usb_storage_transfer_sectors(1) * 512u, 1024u * 1024u);
    CHECK_EQ(usb_storage_transfer_sectors(0), 240u);
    CHECK(usb_storage_transfer_sectors(0) <= 0xFFFFu);
    CHECK(usb_storage_transfer_sectors(1) <= 0xFFFFu);
}

TEST(usb_storage, a_data_phase_is_divided_at_every_64k_boundary_and_nowhere_else) {
    CHECK_EQ(usb_storage_next_piece(0x100000, 1024u * 1024u), 65536u);
    CHECK_EQ(usb_storage_next_piece(0x100000, 4096u), 4096u);
    CHECK_EQ(usb_storage_next_piece(0x100000, 65536u), 65536u);
    CHECK_EQ(usb_storage_next_piece(0x10F000, 65536u), 4096u);
    CHECK_EQ(usb_storage_next_piece(0x10FE00, 512u), 512u);
    CHECK_EQ(usb_storage_next_piece(0x10FFFF, 2u), 1u);

    uint32_t transfer = 240u * 512u;
    uint64_t phys = 0x200000;
    uint32_t done = 0, pieces = 0;
    while (done < transfer) {
        uint32_t piece = usb_storage_next_piece(phys + done, transfer - done);
        CHECK(piece > 0);
        CHECK_EQ((phys + done) / 65536u, (phys + done + piece - 1) / 65536u);
        if (done + piece < transfer) {
            CHECK_EQ(piece % 1024u, 0u);
        }
        done += piece;
        pieces++;
    }
    CHECK_EQ(done, transfer);
    CHECK_EQ(pieces, 2u);
}
