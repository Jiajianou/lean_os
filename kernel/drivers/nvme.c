#include "nvme.h"

#include "drivers/kernel_log.h"
#include "drivers/nvme_split.h"
#include "drivers/pci.h"
#include "library/kernel_library.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"

#define NVME_CLASS    0x01
#define NVME_SUBCLASS 0x08
#define NVME_PROG_IF  0x02

#define REG_CAP    0x00
#define REG_VS     0x08
#define REG_INTMS  0x0C
#define REG_INTMC  0x10
#define REG_CC     0x14
#define REG_CSTS   0x1C
#define REG_AQA    0x24
#define REG_ASQ    0x28
#define REG_ACQ    0x30
#define REG_DOORBELL_BASE 0x1000

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)

#define OPC_ADMIN_CREATE_SQ 0x01
#define OPC_ADMIN_CREATE_CQ 0x05
#define OPC_ADMIN_IDENTIFY  0x06
#define OPC_ADMIN_SET_FEAT  0x09
#define OPC_IO_WRITE        0x01
#define OPC_IO_READ         0x02

#define QUEUE_DEPTH 32
#define IO_QID      1

#define SECTOR_SIZE 512
#define BOUNCE_SECTORS 128
#define BOUNCE_BYTES   (BOUNCE_SECTORS * SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_sqe_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} nvme_cqe_t;

typedef struct {
    volatile nvme_sqe_t *sq;
    volatile nvme_cqe_t *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  phase;
    uint32_t id;
} nvme_queue_t;

static volatile uint8_t *regs;
static uint32_t doorbell_stride;
static nvme_queue_t admin_q, io_q;
static int present;
static uint32_t errors;
static uint16_t next_cid = 1;

static uint32_t nsid;
static uint64_t ns_blocks;
static uint32_t ns_block_size;
static uint32_t ns_shift;

static uint8_t *bounce;
static uint64_t bounce_phys;
static uint64_t prp_list_phys;
static uint8_t *scratch;
static uint64_t scratch_phys;

static uint32_t reg_read32(uint32_t off) {
    return *(volatile uint32_t *)(regs + off);
}

static void reg_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(regs + off) = val;
}

static uint64_t reg_read64(uint32_t off) {
    return (uint64_t)reg_read32(off) | ((uint64_t)reg_read32(off + 4) << 32);
}

static void reg_write64(uint32_t off, uint64_t val) {
    reg_write32(off, (uint32_t)(val & 0xFFFFFFFFu));
    reg_write32(off + 4, (uint32_t)(val >> 32));
}

static void ring_sq(const nvme_queue_t *q, uint16_t tail) {
    *(volatile uint32_t *)(regs + REG_DOORBELL_BASE + (2 * q->id) * doorbell_stride) = tail;
}

static void ring_cq(const nvme_queue_t *q, uint16_t head) {
    *(volatile uint32_t *)(regs + REG_DOORBELL_BASE + (2 * q->id + 1) * doorbell_stride) = head;
}

#define SPIN_LIMIT 100000000u

static uint16_t submit_sync(nvme_queue_t *q, const nvme_sqe_t *cmd) {
    volatile nvme_sqe_t *slot = &q->sq[q->sq_tail];
    k_memcpy((void *)slot, cmd, sizeof(*cmd));
    slot->cid = next_cid++;

    q->sq_tail = (uint16_t)((q->sq_tail + 1) % QUEUE_DEPTH);
    ring_sq(q, q->sq_tail);

    volatile nvme_cqe_t *cqe = &q->cq[q->cq_head];
    uint32_t spins = 0;
    while (((cqe->status & 1u) != q->phase)) {
        if (++spins >= SPIN_LIMIT || (reg_read32(REG_CSTS) & CSTS_CFS)) {
            errors++;
            return 0xFFFF;
        }
        __asm__ volatile("pause");
    }

    uint16_t status = (uint16_t)(cqe->status >> 1);
    q->cq_head = (uint16_t)((q->cq_head + 1) % QUEUE_DEPTH);
    if (q->cq_head == 0) {
        q->phase ^= 1;
    }
    ring_cq(q, q->cq_head);

    if (status != 0) {
        errors++;
    }
    return status;
}

static int alloc_queue(nvme_queue_t *q, uint32_t id) {
    q->sq_phys = pmm_alloc_contiguous(1);
    q->cq_phys = pmm_alloc_contiguous(1);
    if (!q->sq_phys || !q->cq_phys) {
        return -1;
    }
    k_memset((void *)q->sq_phys, 0, 4096);
    k_memset((void *)q->cq_phys, 0, 4096);
    q->sq = (volatile nvme_sqe_t *)q->sq_phys;
    q->cq = (volatile nvme_cqe_t *)q->cq_phys;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;
    q->id = id;
    return 0;
}

static int wait_ready(int want) {
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        uint32_t csts = reg_read32(REG_CSTS);
        if (csts & CSTS_CFS) {
            return -1;
        }
        if (!!(csts & CSTS_RDY) == want) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int identify(uint32_t cns, uint32_t id, uint64_t dest_phys) {
    nvme_sqe_t cmd;
    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OPC_ADMIN_IDENTIFY;
    cmd.nsid = id;
    cmd.prp1 = dest_phys;
    cmd.cdw10 = cns;
    return submit_sync(&admin_q, &cmd) == 0 ? 0 : -1;
}

static int create_io_queues(void) {
    nvme_sqe_t cmd;
    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OPC_ADMIN_CREATE_CQ;
    cmd.prp1 = io_q.cq_phys;
    cmd.cdw10 = (uint32_t)IO_QID | ((QUEUE_DEPTH - 1) << 16);
    cmd.cdw11 = 1;
    if (submit_sync(&admin_q, &cmd) != 0) {
        return -1;
    }

    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OPC_ADMIN_CREATE_SQ;
    cmd.prp1 = io_q.sq_phys;
    cmd.cdw10 = (uint32_t)IO_QID | ((QUEUE_DEPTH - 1) << 16);
    cmd.cdw11 = 1u | ((uint32_t)IO_QID << 16);
    if (submit_sync(&admin_q, &cmd) != 0) {
        return -1;
    }
    return 0;
}

int nvme_init(void) {
    pci_device_t dev;
    for (uint32_t index = 0; index < 8; index++) {
        if (!pci_find_class(NVME_CLASS, NVME_SUBCLASS, NVME_PROG_IF, index, &dev)) {
            return 0;
        }
        pci_enable_device(&dev);

        uint64_t bar = pci_bar_mem_base(&dev, 0);
        uint64_t bar_len = pci_bar_mem_size(&dev, 0);
        if (bar == 0 || bar_len == 0) {
            continue;
        }
        regs = (volatile uint8_t *)vmm_map_mmio(bar, bar_len);
        if (regs == 0) {
            klog_puts("[nvme] BAR0 is not mappable - declining this controller.\n");
            continue;
        }

        uint64_t cap = reg_read64(REG_CAP);
        doorbell_stride = 4u << ((cap >> 32) & 0xF);
        uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
        uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xF);
        if (mqes < QUEUE_DEPTH) {
            klog_puts("[nvme] the controller's maximum queue is shorter than this driver's - declining.\n");
            continue;
        }
        if (mpsmin != 0) {
            klog_puts("[nvme] the controller's minimum page size is not 4 KiB - declining.\n");
            continue;
        }

        reg_write32(REG_CC, reg_read32(REG_CC) & ~CC_EN);
        if (wait_ready(0) != 0) {
            klog_puts("[nvme] the controller never became not-ready - declining.\n");
            continue;
        }
        reg_write32(REG_INTMS, 0xFFFFFFFFu);

        if (alloc_queue(&admin_q, 0) != 0 || alloc_queue(&io_q, IO_QID) != 0) {
            klog_puts("[nvme] no memory for the queues - declining.\n");
            continue;
        }

        reg_write32(REG_AQA, ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1));
        reg_write64(REG_ASQ, admin_q.sq_phys);
        reg_write64(REG_ACQ, admin_q.cq_phys);

        uint32_t cc = CC_EN | (6u << 16) | (4u << 20);
        reg_write32(REG_CC, cc);
        if (wait_ready(1) != 0) {
            klog_puts("[nvme] the controller never became ready - declining.\n");
            continue;
        }

        bounce_phys = pmm_alloc_contiguous(BOUNCE_BYTES / 4096);
        bounce = (uint8_t *)bounce_phys;
        scratch_phys = pmm_alloc_contiguous(1);
        scratch = (uint8_t *)scratch_phys;
        prp_list_phys = pmm_alloc_contiguous(1);
        if (!bounce_phys || !scratch_phys || !prp_list_phys) {
            klog_puts("[nvme] no memory for the transfer buffers - declining.\n");
            continue;
        }
        uint64_t *prp = (uint64_t *)prp_list_phys;
        k_memset(prp, 0, 4096);
        for (uint32_t i = 1; i < BOUNCE_BYTES / 4096; i++) {
            prp[i - 1] = bounce_phys + (uint64_t)i * 4096;
        }

        if (create_io_queues() != 0) {
            klog_puts("[nvme] the controller refused to create an I/O queue - declining.\n");
            continue;
        }

        int got_ns = 0;
        for (uint32_t candidate = 1; candidate <= 8 && !got_ns; candidate++) {
            if (identify(0, candidate, bounce_phys) != 0) {
                continue;
            }
            uint64_t nsze = *(const uint64_t *)(bounce + 0);
            if (nsze == 0) {
                continue;
            }
            uint8_t flbas = bounce[26];
            const uint32_t *lbaf = (const uint32_t *)(bounce + 128);
            uint32_t fmt = lbaf[flbas & 0x0F];
            uint32_t lbads = (fmt >> 16) & 0xFF;
            if (lbads < 9 || lbads > 12) {
                klog_puts("[nvme] namespace block size is not between 512 and 4096 - skipping it.\n");
                continue;
            }
            nsid = candidate;
            ns_blocks = nsze;
            ns_block_size = 1u << lbads;
            ns_shift = lbads - 9;
            got_ns = 1;
        }
        if (!got_ns) {
            klog_puts("[nvme] the controller has no active namespace - declining.\n");
            continue;
        }

        present = 1;
        klog_puts("[nvme] NVMe ");
        klog_put_hex32(reg_read32(REG_VS));
        klog_puts(", namespace ");
        klog_put_dec(nsid);
        klog_puts(", ");
        klog_put_dec(ns_blocks << ns_shift);
        klog_puts(" sectors of 512 (");
        klog_put_dec(ns_block_size);
        klog_puts("-byte blocks), doorbell stride ");
        klog_put_dec(doorbell_stride);
        klog_putc('\n');
        return 1;
    }
    return 0;
}

static int io_command(int write, uint64_t block, uint32_t blocks, uint32_t bytes,
                      uint64_t data_phys) {
    nvme_sqe_t cmd;
    k_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = write ? OPC_IO_WRITE : OPC_IO_READ;
    cmd.nsid = nsid;
    cmd.prp1 = data_phys;
    if (bytes > 4096) {
        cmd.prp2 = (bytes <= 8192 && data_phys == bounce_phys)
                       ? bounce_phys + 4096
                       : prp_list_phys;
    }
    cmd.cdw10 = (uint32_t)(block & 0xFFFFFFFFu);
    cmd.cdw11 = (uint32_t)(block >> 32);
    cmd.cdw12 = blocks - 1;

    uint16_t status = submit_sync(&io_q, &cmd);
    if (status != 0) {
        klog_puts("[nvme] ");
        klog_puts(write ? "write" : "read");
        klog_puts(" failed at block 0x");
        klog_put_hex64(block);
        klog_puts(", status 0x");
        klog_put_hex32(status);
        klog_putc('\n');
        return -1;
    }
    return 0;
}

static int transfer(uint64_t lba, uint32_t count, uint8_t *buf, int write) {
    if (!present) {
        return -1;
    }
    nvme_chunk_t c;
    while (nvme_split_next(lba, count, ns_shift, BOUNCE_SECTORS, &c)) {
        if (c.partial) {
            if (io_command(0, c.block, 1, ns_block_size, scratch_phys) != 0) {
                return -1;
            }
            if (write) {
                k_memcpy(scratch + c.offset * SECTOR_SIZE, buf, c.sectors * SECTOR_SIZE);
                if (io_command(1, c.block, 1, ns_block_size, scratch_phys) != 0) {
                    return -1;
                }
            } else {
                k_memcpy(buf, scratch + c.offset * SECTOR_SIZE, c.sectors * SECTOR_SIZE);
            }
        } else {
            uint32_t bytes = c.sectors * SECTOR_SIZE;
            if (write) {
                k_memcpy(bounce, buf, bytes);
            }
            if (io_command(write, c.block, c.blocks, bytes, bounce_phys) != 0) {
                return -1;
            }
            if (!write) {
                k_memcpy(buf, bounce, bytes);
            }
        }
        buf += c.sectors * SECTOR_SIZE;
        lba += c.sectors;
        count -= c.sectors;
    }
    return 0;
}

int nvme_read(uint64_t lba, uint32_t count, void *buf) {
    return transfer(lba, count, (uint8_t *)buf, 0);
}

int nvme_write(uint64_t lba, uint32_t count, const void *buf) {
    return transfer(lba, count, (uint8_t *)(uintptr_t)buf, 1);
}
