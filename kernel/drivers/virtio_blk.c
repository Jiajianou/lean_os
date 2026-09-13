#include "virtio_blk.h"

#include "arch/x86_64/io.h"
#include "drivers/klog.h"
#include "drivers/pci.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "panic.h"

#define VIRTIO_VENDOR      0x1AF4
#define VIRTIO_BLK_DEVICE  0x1001

#define VIO_HOST_FEATURES  0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN      0x08
#define VIO_QUEUE_SIZE     0x0C
#define VIO_QUEUE_SELECT   0x0E
#define VIO_QUEUE_NOTIFY   0x10
#define VIO_STATUS         0x12
#define VIO_ISR            0x13
#define VIO_BLK_CAPACITY   0x14

#define VIO_STATUS_ACK     0x01
#define VIO_STATUS_DRIVER  0x02
#define VIO_STATUS_DRIVER_OK 0x04
#define VIO_STATUS_FAILED  0x80

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN    0
#define VIRTIO_BLK_T_OUT   1
#define VIRTIO_BLK_T_FLUSH 4

#define SECTOR_SIZE 512

#define BOUNCE_SECTORS 128
#define BOUNCE_BYTES   (BOUNCE_SECTORS * SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} vring_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[];
} vring_used_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_t;

static uint16_t io_base;
static uint16_t queue_size;
static int present;
static uint64_t capacity_sectors;

static volatile vring_desc_t *desc;
static volatile vring_avail_t *avail;
static volatile vring_used_t *used;
static uint64_t ring_phys;
static uint32_t ring_frames;

static virtio_blk_req_t *req_hdr;
static volatile uint8_t *req_status;
static uint8_t *bounce;
static uint64_t hdr_phys, bounce_phys;

static uint16_t last_used_idx;

static uint64_t ring_bytes(uint16_t qsize) {
    uint64_t a = (uint64_t)qsize * sizeof(vring_desc_t) +
                 (uint64_t)(3 + qsize) * sizeof(uint16_t);
    uint64_t aligned = (a + 4095) & ~4095ULL;
    uint64_t b = (uint64_t)(3 * sizeof(uint16_t)) +
                 (uint64_t)qsize * sizeof(vring_used_elem_t);
    return aligned + b;
}

int virtio_blk_init(void) {
    pci_device_t dev;
    if (!pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_DEVICE, &dev)) {
        return 0;
    }
    pci_enable_device(&dev);
    io_base = pci_bar0_io_base(&dev);
    if (io_base == 0) {
        klog_puts("[virtio-blk] BAR0 is memory-mapped - falling back.\n");
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

    capacity_sectors = (uint64_t)inl((uint16_t)(io_base + VIO_BLK_CAPACITY)) |
                       ((uint64_t)inl((uint16_t)(io_base + VIO_BLK_CAPACITY + 4)) << 32);

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
    ring_phys = pmm_alloc_contiguous(ring_frames);
    k_memset((void *)ring_phys, 0, ring_frames * 4096);
    desc = (volatile vring_desc_t *)ring_phys;
    avail = (volatile vring_avail_t *)(ring_phys + (uint64_t)queue_size * sizeof(vring_desc_t));
    uint64_t used_off = ((uint64_t)queue_size * sizeof(vring_desc_t) +
                         (uint64_t)(3 + queue_size) * sizeof(uint16_t) + 4095) & ~4095ULL;
    used = (volatile vring_used_t *)(ring_phys + used_off);

    uint64_t hdr_page = pmm_alloc_contiguous(1);
    k_memset((void *)hdr_page, 0, 4096);
    req_hdr = (virtio_blk_req_t *)hdr_page;
    req_status = (volatile uint8_t *)(hdr_page + sizeof(virtio_blk_req_t));
    hdr_phys = hdr_page;

    bounce_phys = pmm_alloc_contiguous(BOUNCE_BYTES / 4096);
    bounce = (uint8_t *)bounce_phys;

    avail->flags = 1;

    outl((uint16_t)(io_base + VIO_QUEUE_PFN), (uint32_t)(ring_phys >> 12));
    outb((uint16_t)(io_base + VIO_STATUS),
         VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_DRIVER_OK);

    last_used_idx = used->idx;
    present = 1;

    klog_puts("[virtio-blk] ");
    klog_put_hex64(capacity_sectors);
    klog_puts(" sectors, queue ");
    klog_put_hex32(queue_size);
    klog_puts(", ring at 0x");
    klog_put_hex64(ring_phys);
    klog_putc('\n');
    return 1;
}

uint64_t virtio_blk_capacity(void) {
    return present ? capacity_sectors : 0;
}

static uint32_t errors;

uint32_t virtio_blk_error_count(void) {
    return errors;
}

static int submit(uint32_t type, uint64_t sector, uint32_t len, int device_writes) {
    req_hdr->type = type;
    req_hdr->reserved = 0;
    req_hdr->sector = sector;
    *req_status = 0xFF;

    uint16_t status_desc = len > 0 ? 2 : 1;

    desc[0].addr = hdr_phys;
    desc[0].len = sizeof(virtio_blk_req_t);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next = 1;

    if (len > 0) {
        desc[1].addr = bounce_phys;
        desc[1].len = len;
        desc[1].flags = (uint16_t)(VRING_DESC_F_NEXT | (device_writes ? VRING_DESC_F_WRITE : 0));
        desc[1].next = 2;
    }

    desc[status_desc].addr = hdr_phys + sizeof(virtio_blk_req_t);
    desc[status_desc].len = 1;
    desc[status_desc].flags = VRING_DESC_F_WRITE;
    desc[status_desc].next = 0;

    avail->ring[avail->idx % queue_size] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx = (uint16_t)(avail->idx + 1);
    __asm__ volatile("" ::: "memory");
    outw((uint16_t)(io_base + VIO_QUEUE_NOTIFY), 0);

    for (uint32_t spin = 0; spin < 200000000u; spin++) {
        if (used->idx != last_used_idx) {
            last_used_idx = used->idx;
            (void)inb((uint16_t)(io_base + VIO_ISR));
            if (*req_status != 0) {
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

int virtio_blk_read(uint64_t lba, uint32_t count, void *buf) {
    uint8_t *dst = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        if (submit(VIRTIO_BLK_T_IN, lba, n * SECTOR_SIZE, 1) != 0) {
            return -1;
        }
        k_memcpy(dst, bounce, n * SECTOR_SIZE);
        dst += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *src = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        k_memcpy(bounce, src, n * SECTOR_SIZE);
        if (submit(VIRTIO_BLK_T_OUT, lba, n * SECTOR_SIZE, 0) != 0) {
            return -1;
        }
        src += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return submit(VIRTIO_BLK_T_FLUSH, 0, 0, 0);
}
