#include "virtio_blk.h"

#include "arch/x86_64/io.h"
#include "drivers/klog.h"
#include "drivers/pci.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "panic.h"

/* PCI ids. 0x1AF4 is Red Hat's virtio vendor id; 0x1001 is the legacy
 * block device. A transitional device (which is what QEMU presents by
 * default on the `pc` machine) answers to this id and speaks the legacy
 * transport below, which is why this driver does not have to know
 * anything about virtio 1.0's capability structures. */
#define VIRTIO_VENDOR      0x1AF4
#define VIRTIO_BLK_DEVICE  0x1001

/* Legacy virtio-pci register window, offsets from BAR0. */
#define VIO_HOST_FEATURES  0x00 /* 32-bit, read */
#define VIO_GUEST_FEATURES 0x04 /* 32-bit, write */
#define VIO_QUEUE_PFN      0x08 /* 32-bit: ring physical address >> 12 */
#define VIO_QUEUE_SIZE     0x0C /* 16-bit, read */
#define VIO_QUEUE_SELECT   0x0E /* 16-bit, write */
#define VIO_QUEUE_NOTIFY   0x10 /* 16-bit, write */
#define VIO_STATUS         0x12 /* 8-bit */
#define VIO_ISR            0x13 /* 8-bit, read-to-clear */
#define VIO_BLK_CAPACITY   0x14 /* 64-bit, device-specific config */

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

/* How much of one request this driver will move at a time. The bounce
 * buffer below is this big, and a caller asking for more is split into
 * several requests - see the note on bouncing for why there is a buffer
 * at all. 64 KiB is 128 sectors, which is comfortably more than any
 * single leanfs operation asks for and small enough to be an ordinary
 * contiguous allocation. */
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

/* The ring, and everything the device reads or writes by physical
 * address. All of it comes from pmm_alloc_contiguous, which since M90
 * always allocates below 4 GiB - the legacy transport hands the device a
 * 32-bit page frame number for the ring, and there is no reason for the
 * buffers to live anywhere else. */
/* `volatile` on all three, and this is not decoration. The device writes
 * used->idx from outside this CPU's view of the program, and the poll
 * loop in submit() reads it in a tight loop with nothing else in the
 * body - exactly the shape a compiler is entitled to hoist out and turn
 * into an infinite loop. The avail ring is volatile for the mirror-image
 * reason: the device reads it, so the writes must actually happen. */
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

/* ---- Why there is a bounce buffer, which is the one design decision in
 * this file worth arguing about ----------------------------------------
 *
 * The device is handed physical addresses. A caller's buffer is a kernel
 * virtual address, and since M90 the kernel heap lives at 256 GiB - which
 * is emphatically not identity-mapped, so its physical pages are neither
 * contiguous nor derivable from the pointer. Translating would mean
 * walking the page tables per page and building a scatter list, which is
 * a real feature (and the right one eventually).
 *
 * A copy is not the cost it looks like next to what it replaces. PIO
 * moved every byte through a port instruction; this moves every byte
 * through a memcpy, which is one to two orders of magnitude cheaper, and
 * then the device DMAs it with no CPU involvement at all. The honest way
 * to say it: this milestone removes the port I/O, not the copy, and the
 * copy is what the next one can remove if a measurement asks. */

static uint64_t ring_bytes(uint16_t qsize) {
    /* Legacy layout: descriptors, then avail (with its used_event), then
     * padding to the next 4 KiB, then used (with its avail_event). */
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
        /* Q16: a memory-mapped BAR. blk_init falls back to ATA, which is
         * exactly the path a machine with no virtio device takes - so
         * "this card is addressed a way this driver cannot use" and
         * "there is no card" arrive at the same, working, place. */
        klog_puts("[virtio-blk] BAR0 is memory-mapped - falling back.\n");
        return 0;
    }
    if (io_base == 0) {
        return 0;
    }

    /* Reset, then acknowledge in the order the spec requires. A device
     * that is never told DRIVER_OK will not touch the ring, which is the
     * failure mode to want if anything below goes wrong. */
    outb((uint16_t)(io_base + VIO_STATUS), 0);
    outb((uint16_t)(io_base + VIO_STATUS), VIO_STATUS_ACK);
    outb((uint16_t)(io_base + VIO_STATUS), VIO_STATUS_ACK | VIO_STATUS_DRIVER);

    /* No feature is negotiated. Every optional virtio-blk feature is
     * about doing more than this driver does - barriers, discard, write
     * zeroes, multiqueue - and accepting one would be promising the
     * device something. Zero is the honest answer and the device is
     * required to work with it. */
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
    /* Three descriptors per request is all this driver ever uses, so a
     * huge queue buys nothing and a tiny one has to be refused. */
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

    /* One page for the request header and the status byte, and the
     * bounce buffer after it. Separate allocations would be two more
     * physical addresses to track for no benefit. */
    uint64_t hdr_page = pmm_alloc_contiguous(1);
    k_memset((void *)hdr_page, 0, 4096);
    req_hdr = (virtio_blk_req_t *)hdr_page;
    req_status = (volatile uint8_t *)(hdr_page + sizeof(virtio_blk_req_t));
    hdr_phys = hdr_page;

    bounce_phys = pmm_alloc_contiguous(BOUNCE_BYTES / 4096);
    bounce = (uint8_t *)bounce_phys;

    /* VRING_AVAIL_F_NO_INTERRUPT. This driver polls, and nothing in this
     * kernel is registered on the PCI interrupt this device would
     * otherwise raise - telling the device not to bother is better than
     * relying on an unhandled line behaving itself, and it is the one
     * flag a polling driver owes the device. */
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

/* One request: header descriptor, data descriptor, status descriptor,
 * chained. Descriptors 0/1/2 every time, because this driver has exactly
 * one request in flight and a free-list over three entries would be
 * bookkeeping with nothing to book. */
static uint32_t errors;

uint32_t virtio_blk_error_count(void) {
    return errors;
}

static int submit(uint32_t type, uint64_t sector, uint32_t len, int device_writes) {
    req_hdr->type = type;
    req_hdr->reserved = 0;
    req_hdr->sector = sector;
    *req_status = 0xFF;

    /* Three descriptors for a read or a write, two for a flush. A flush
     * carries no data, and a zero-length descriptor in the middle of a
     * chain is not something the specification asks a device to accept -
     * so the chain is built to the shape of the request rather than
     * padded to a fixed one. */
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
    /* The device may read avail->idx at any moment, so the ring entry has
     * to be visible before the index that publishes it. On x86 stores are
     * not reordered with other stores, so a compiler barrier is the whole
     * requirement here - written down rather than relied on silently,
     * because it is the one thing in this file that is true because of
     * the architecture rather than because of the code. */
    __asm__ volatile("" ::: "memory");
    avail->idx = (uint16_t)(avail->idx + 1);
    __asm__ volatile("" ::: "memory");
    outw((uint16_t)(io_base + VIO_QUEUE_NOTIFY), 0);

    /* Polled. See the header for why there is no interrupt here.
     *
     * Q16: the bound used to end in a panic, "for the same reason ata.c's
     * does: a wedged disk is not something this kernel can carry on
     * around". That was an argument rather than a measurement, and it is
     * wrong in the direction that costs most: a machine that halts on a
     * bad sector loses the desktop, the unsaved editor buffer and the
     * chance to say what happened, in exchange for nothing. It reports
     * now, and the caller decides.
     *
     * The stale used-index is deliberately NOT resynchronised on a
     * timeout. A request that never completed may complete later, and a
     * driver that moved past it would then read somebody else's
     * completion as its own - so `last_used_idx` stays where it is and
     * the next request notices the extra entry. One request in flight is
     * what makes that safe. */
    for (uint32_t spin = 0; spin < 200000000u; spin++) {
        if (used->idx != last_used_idx) {
            last_used_idx = used->idx;
            (void)inb((uint16_t)(io_base + VIO_ISR)); /* read-to-clear */
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
    /* M71's ordering guarantee is what makes the absence of a journal
     * defensible, and it is a guarantee about what has reached the disk.
     * A flush after every write is what makes it still true through a
     * device that has a cache of its own.
     *
     * Q16: and its answer is the call's answer. A write reported
     * successful whose flush failed is precisely the case that ordering
     * guarantee cannot survive. */
    return submit(VIRTIO_BLK_T_FLUSH, 0, 0, 0);
}
