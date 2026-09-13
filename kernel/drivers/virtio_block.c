#include "virtio_block.h"

#include "architecture/x86_64/io.h"
#include "drivers/kernel_log.h"
#include "drivers/pci.h"
#include "library/kernel_library.h"
#include "memory_management/physical_memory.h"
#include "panic.h"

#define VIRTIO_VENDOR      0x1AF4
#define VIRTIO_BLOCK_DEVICE_DEVICE  0x1001

#define VIO_HOST_FEATURES  0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN      0x08
#define VIO_QUEUE_SIZE     0x0C
#define VIO_QUEUE_SELECT   0x0E
#define VIO_QUEUE_NOTIFY   0x10
#define VIO_STATUS         0x12
#define VIO_ISR            0x13
#define VIO_BLOCK_DEVICE_CAPACITY   0x14

#define VIO_STATUS_ACK     0x01
#define VIO_STATUS_DRIVER  0x02
#define VIO_STATUS_DRIVER_OK 0x04
#define VIO_STATUS_FAILED  0x80

#define VRING_DESCRIPTOR_F_NEXT  1
#define VRING_DESCRIPTOR_F_WRITE 2

#define VIRTIO_BLOCK_DEVICE_T_IN    0
#define VIRTIO_BLOCK_DEVICE_T_OUT   1
#define VIRTIO_BLOCK_DEVICE_T_FLUSH 4

#define SECTOR_SIZE 512

#define BOUNCE_SECTORS 128
#define BOUNCE_BYTES   (BOUNCE_SECTORS * SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} vring_descriptor_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} vring_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t length;
} vring_used_element_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vring_used_element_t ring[];
} vring_used_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_block_device_request_t;

static uint16_t io_base;
static uint16_t queue_size;
static int present;
static uint64_t capacity_sectors;

static volatile vring_descriptor_t *descriptor;
static volatile vring_avail_t *avail;
static volatile vring_used_t *used;
static uint64_t ring_phys;
static uint32_t ring_frames;

static virtio_block_device_request_t *request_header;
static volatile uint8_t *request_status;
static uint8_t *bounce;
static uint64_t header_phys, bounce_phys;

static uint16_t last_used_index;

static uint64_t ring_bytes(uint16_t qsize) {
    uint64_t a = (uint64_t)qsize * sizeof(vring_descriptor_t) +
                 (uint64_t)(3 + qsize) * sizeof(uint16_t);
    uint64_t aligned = (a + 4095) & ~4095ULL;
    uint64_t b = (uint64_t)(3 * sizeof(uint16_t)) +
                 (uint64_t)qsize * sizeof(vring_used_element_t);
    return aligned + b;
}

int virtio_block_device_init(void) {
    pci_device_t dev;
    if (!pci_find_device(VIRTIO_VENDOR, VIRTIO_BLOCK_DEVICE_DEVICE, &dev)) {
        return 0;
    }
    pci_enable_device(&dev);
    io_base = pci_bar0_io_base(&dev);
    if (io_base == 0) {
        kernel_log_puts("[virtio-blk] BAR0 is memory-mapped - falling back.\n");
        return 0;
    }
    if (io_base == 0) {
        return 0;
    }

    outb((uint16_t)(io_base + VIO_STATUS), 0);
    outb((uint16_t)(io_base + VIO_STATUS), VIO_STATUS_ACK);
    outb((uint16_t)(io_base + VIO_STATUS), VIO_STATUS_ACK | VIO_STATUS_DRIVER);

    (void)inl((uint16_t)(io_base + VIO_HOST_FEATURES));
    outl((uint16_t)(io_base + VIO_GUEST_FEATURES), 0);

    capacity_sectors = (uint64_t)inl((uint16_t)(io_base + VIO_BLOCK_DEVICE_CAPACITY)) |
                       ((uint64_t)inl((uint16_t)(io_base + VIO_BLOCK_DEVICE_CAPACITY + 4)) << 32);

    outw((uint16_t)(io_base + VIO_QUEUE_SELECT), 0);
    queue_size = inw((uint16_t)(io_base + VIO_QUEUE_SIZE));
    if (queue_size == 0) {
        outb((uint16_t)(io_base + VIO_STATUS), VIO_STATUS_FAILED);
        return 0;
    }
    if (queue_size < 8) {
        outb((uint16_t)(io_base + VIO_STATUS), VIO_STATUS_FAILED);
        return 0;
    }

    uint64_t bytes = ring_bytes(queue_size);
    ring_frames = (uint32_t)((bytes + 4095) / 4096);
    ring_phys = physical_memory_alloc_contiguous(ring_frames);
    k_memset((void *)ring_phys, 0, ring_frames * 4096);
    descriptor = (volatile vring_descriptor_t *)ring_phys;
    avail = (volatile vring_avail_t *)(ring_phys + (uint64_t)queue_size * sizeof(vring_descriptor_t));
    uint64_t used_off = ((uint64_t)queue_size * sizeof(vring_descriptor_t) +
                         (uint64_t)(3 + queue_size) * sizeof(uint16_t) + 4095) & ~4095ULL;
    used = (volatile vring_used_t *)(ring_phys + used_off);

    uint64_t header_page = physical_memory_alloc_contiguous(1);
    k_memset((void *)header_page, 0, 4096);
    request_header = (virtio_block_device_request_t *)header_page;
    request_status = (volatile uint8_t *)(header_page + sizeof(virtio_block_device_request_t));
    header_phys = header_page;

    bounce_phys = physical_memory_alloc_contiguous(BOUNCE_BYTES / 4096);
    bounce = (uint8_t *)bounce_phys;

    avail->flags = 1;

    outl((uint16_t)(io_base + VIO_QUEUE_PFN), (uint32_t)(ring_phys >> 12));
    outb((uint16_t)(io_base + VIO_STATUS),
         VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_DRIVER_OK);

    last_used_index = used->idx;
    present = 1;

    kernel_log_puts("[virtio-blk] ");
    kernel_log_put_hex64(capacity_sectors);
    kernel_log_puts(" sectors, queue ");
    kernel_log_put_hex32(queue_size);
    kernel_log_puts(", ring at 0x");
    kernel_log_put_hex64(ring_phys);
    kernel_log_putc('\n');
    return 1;
}

static uint32_t errors;

static int submit(uint32_t type, uint64_t sector, uint32_t length, int device_writes) {
    request_header->type = type;
    request_header->reserved = 0;
    request_header->sector = sector;
    *request_status = 0xFF;

    uint16_t status_descriptor = length > 0 ? 2 : 1;

    descriptor[0].address = header_phys;
    descriptor[0].length = sizeof(virtio_block_device_request_t);
    descriptor[0].flags = VRING_DESCRIPTOR_F_NEXT;
    descriptor[0].next = 1;

    if (length > 0) {
        descriptor[1].address = bounce_phys;
        descriptor[1].length = length;
        descriptor[1].flags = (uint16_t)(VRING_DESCRIPTOR_F_NEXT | (device_writes ? VRING_DESCRIPTOR_F_WRITE : 0));
        descriptor[1].next = 2;
    }

    descriptor[status_descriptor].address = header_phys + sizeof(virtio_block_device_request_t);
    descriptor[status_descriptor].length = 1;
    descriptor[status_descriptor].flags = VRING_DESCRIPTOR_F_WRITE;
    descriptor[status_descriptor].next = 0;

    avail->ring[avail->idx % queue_size] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx = (uint16_t)(avail->idx + 1);
    __asm__ volatile("" ::: "memory");
    outw((uint16_t)(io_base + VIO_QUEUE_NOTIFY), 0);

    for (uint32_t spin = 0; spin < 200000000u; spin++) {
        if (used->idx != last_used_index) {
            last_used_index = used->idx;
            (void)inb((uint16_t)(io_base + VIO_ISR));
            if (*request_status != 0) {
                errors++;
                return -1;
            }
            return 0;
        }
        __asm__ volatile("pause");
    }
    errors++;
    return -1;
}

int virtio_block_device_read(uint64_t lba, uint32_t count, void *buffer) {
    uint8_t *destination = (uint8_t *)buffer;
    while (count > 0) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        if (submit(VIRTIO_BLOCK_DEVICE_T_IN, lba, n * SECTOR_SIZE, 1) != 0) {
            return -1;
        }
        k_memcpy(destination, bounce, n * SECTOR_SIZE);
        destination += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

int virtio_block_device_write(uint64_t lba, uint32_t count, const void *buffer) {
    const uint8_t *source = (const uint8_t *)buffer;
    while (count > 0) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        k_memcpy(bounce, source, n * SECTOR_SIZE);
        if (submit(VIRTIO_BLOCK_DEVICE_T_OUT, lba, n * SECTOR_SIZE, 0) != 0) {
            return -1;
        }
        source += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return submit(VIRTIO_BLOCK_DEVICE_T_FLUSH, 0, 0, 0);
}
