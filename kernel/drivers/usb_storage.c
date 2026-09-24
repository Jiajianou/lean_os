#include "usb_storage.h"

#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "drivers/usb_storage_protocol.h"
#include "drivers/xhci.h"
#include "library/kernel_library.h"
#include "memory_management/physical_memory.h"

#define SECTOR_BYTES 512

#define MAX_TRANSFER_SECTORS 8

#define READY_ATTEMPTS 16

static int present;
static uint64_t sectors;
static uint32_t next_tag = 1;

static uint8_t *wrapper_buffer;
static uint64_t wrapper_buffer_phys;
static uint8_t *data_buffer;
static uint64_t data_buffer_phys;

static int run_command(const uint8_t *command, uint8_t command_length, uint8_t direction,
                       uint32_t transfer_length) {
    uint32_t tag = next_tag++;

    uint32_t wrapper_length = usb_storage_build_command_wrapper(
        wrapper_buffer, tag, transfer_length, direction, 0, command, command_length);
    if (wrapper_length == 0) {
        return 0;
    }

    uint32_t moved = 0;
    if (xhci_bulk_out(wrapper_buffer_phys, wrapper_length, &moved) != 0 ||
        moved != wrapper_length) {
        return 0;
    }

    if (transfer_length > 0) {
        if (direction == USB_STORAGE_DIRECTION_IN) {
            if (xhci_bulk_in(data_buffer_phys, transfer_length, &moved) != 0) {
                return 0;
            }
        } else {
            if (xhci_bulk_out(data_buffer_phys, transfer_length, &moved) != 0) {
                return 0;
            }
        }
    }

    k_memset(wrapper_buffer, 0, USB_STORAGE_CSW_LENGTH);
    if (xhci_bulk_in(wrapper_buffer_phys, USB_STORAGE_CSW_LENGTH, &moved) != 0) {
        return 0;
    }
    return usb_storage_status_wrapper_ok(wrapper_buffer, moved, tag, 0);
}

int usb_storage_present(void) {
    return present;
}

uint64_t usb_storage_sector_count(void) {
    return sectors;
}

int usb_storage_read(uint64_t lba, uint32_t count, void *buffer) {
    if (!present || !buffer || count == 0) {
        return 0;
    }
    uint8_t *out = (uint8_t *)buffer;
    while (count > 0) {
        uint32_t chunk = count > MAX_TRANSFER_SECTORS ? MAX_TRANSFER_SECTORS : count;
        if (lba + chunk > sectors) {
            return 0;
        }
        uint8_t command[10];
        uint32_t command_length =
            usb_storage_build_read10(command, (uint32_t)lba, (uint16_t)chunk);
        if (!run_command(command, (uint8_t)command_length, USB_STORAGE_DIRECTION_IN,
                         chunk * SECTOR_BYTES)) {
            return 0;
        }
        k_memcpy(out, data_buffer, chunk * SECTOR_BYTES);
        out += chunk * SECTOR_BYTES;
        lba += chunk;
        count -= chunk;
    }
    return 1;
}

int usb_storage_write(uint64_t lba, uint32_t count, const void *buffer) {
    if (!present || !buffer || count == 0) {
        return 0;
    }
    const uint8_t *in = (const uint8_t *)buffer;
    while (count > 0) {
        uint32_t chunk = count > MAX_TRANSFER_SECTORS ? MAX_TRANSFER_SECTORS : count;
        if (lba + chunk > sectors) {
            return 0;
        }
        k_memcpy(data_buffer, in, chunk * SECTOR_BYTES);
        uint8_t command[10];
        uint32_t command_length =
            usb_storage_build_write10(command, (uint32_t)lba, (uint16_t)chunk);
        if (!run_command(command, (uint8_t)command_length, USB_STORAGE_DIRECTION_OUT,
                         chunk * SECTOR_BYTES)) {
            return 0;
        }
        in += chunk * SECTOR_BYTES;
        lba += chunk;
        count -= chunk;
    }
    return 1;
}

int usb_storage_init(void) {
    present = 0;
    sectors = 0;

    if (!xhci_storage_present()) {
        return 0;
    }

    uint64_t wrapper_page = physical_memory_alloc_contiguous(1);
    uint64_t data_pages = physical_memory_alloc_contiguous(MAX_TRANSFER_SECTORS * SECTOR_BYTES / 4096);
    if (!wrapper_page || !data_pages) {
        return 0;
    }
    wrapper_buffer = (uint8_t *)(uintptr_t)wrapper_page;
    wrapper_buffer_phys = wrapper_page;
    data_buffer = (uint8_t *)(uintptr_t)data_pages;
    data_buffer_phys = data_pages;

    uint8_t command[10];
    uint32_t command_length = usb_storage_build_inquiry(command);
    if (!run_command(command, (uint8_t)command_length, USB_STORAGE_DIRECTION_IN,
                     SCSI_INQUIRY_LENGTH)) {
        kernel_log_puts("[usb-storage] the device would not answer INQUIRY.\n");
        return 0;
    }

    /* A stick that has only just been given power answers "not ready" for a
       while, and a host that believes the first answer decides there is no
       disk on a machine that has one. */
    int ready = 0;
    for (int attempt = 0; attempt < READY_ATTEMPTS && !ready; attempt++) {
        command_length = usb_storage_build_test_unit_ready(command);
        ready = run_command(command, (uint8_t)command_length, USB_STORAGE_DIRECTION_OUT, 0);
        if (!ready) {
            pit_sleep_ms(50);
        }
    }
    if (!ready) {
        kernel_log_puts("[usb-storage] the device never became ready.\n");
        return 0;
    }

    command_length = usb_storage_build_read_capacity10(command);
    if (!run_command(command, (uint8_t)command_length, USB_STORAGE_DIRECTION_IN,
                     SCSI_READ_CAPACITY10_LENGTH)) {
        kernel_log_puts("[usb-storage] the device would not report its capacity.\n");
        return 0;
    }

    uint32_t sector_bytes = 0;
    if (!usb_storage_parse_read_capacity10(data_buffer, SCSI_READ_CAPACITY10_LENGTH, &sectors,
                                           &sector_bytes)) {
        kernel_log_puts("[usb-storage] the capacity this device reports cannot be true.\n");
        return 0;
    }
    if (sector_bytes != SECTOR_BYTES) {
        /* Everything above this driver counts in 512-byte sectors. Pretending
           a 4096-byte device is one of those would read the right bytes from
           the wrong place. */
        kernel_log_puts("[usb-storage] this device's sector is not 512 bytes - declining it.\n");
        sectors = 0;
        return 0;
    }

    present = 1;

    kernel_log_puts("[usb-storage] ");
    kernel_log_put_dec((uint32_t)(sectors / 2048));
    kernel_log_puts(" MiB, ");
    kernel_log_put_dec((uint32_t)sectors);
    kernel_log_puts(" sectors of ");
    kernel_log_put_dec(sector_bytes);
    kernel_log_puts(" bytes.\n");
    return 1;
}
