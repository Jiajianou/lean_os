#include "ahci.h"

#include "drivers/klog.h"
#include "drivers/pci.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* PCI class 01:06:01 - mass storage, SATA, AHCI 1.0 programming
 * interface. The prog-if is what separates an AHCI controller from the
 * same silicon in legacy IDE mode (01:01:xx), which drivers/ata.c
 * already drives and which this driver must not bind to. */
#define AHCI_CLASS    0x01
#define AHCI_SUBCLASS 0x06
#define AHCI_PROG_IF  0x01

/* Generic host control, at the start of the BAR. */
#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_IS   0x08
#define HBA_PI   0x0C
#define HBA_VS   0x10

#define GHC_HR   (1u << 0)  /* HBA reset */
#define GHC_IE   (1u << 1)  /* interrupt enable */
#define GHC_AE   (1u << 31) /* AHCI enable */

/* Port registers: 0x100 + port * 0x80. */
#define PORT_BASE(n) (0x100 + (n) * 0x80)
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SCTL  0x2C
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1u << 0)   /* start: the device may process the command list */
#define CMD_FRE  (1u << 4)   /* FIS receive enable */
#define CMD_FR   (1u << 14)  /* FIS receive running */
#define CMD_CR   (1u << 15)  /* command list running */

#define TFD_ERR  (1u << 0)
#define TFD_DRQ  (1u << 3)
#define TFD_BSY  (1u << 7)

#define SIG_SATA 0x00000101u /* a plain SATA disk. ATAPI is 0xEB140101 */

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC

#define SECTOR_SIZE 512

/* Same 64 KiB bounce buffer virtio_blk.c uses, for exactly the same
 * reason - see the long note there. The kernel heap lives at 256 GiB and
 * is not identity-mapped, so a caller's pointer has no physical address
 * this driver can hand a device without walking page tables. 128 sectors
 * is more than any single leanfs operation asks for. */
#define BOUNCE_SECTORS 128
#define BOUNCE_BYTES   (BOUNCE_SECTORS * SECTOR_SIZE)

/* A command header. 32 of them make a port's command list. */
typedef struct __attribute__((packed)) {
    uint16_t flags;    /* CFL in bits 0-4, W in bit 6, and the rest unused here */
    uint16_t prdtl;    /* PRDT entries in the table below */
    volatile uint32_t prdbc; /* bytes the device actually transferred */
    uint32_t ctba;     /* command table, 128-byte aligned */
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_cmd_header_t;

typedef struct __attribute__((packed)) {
    uint64_t dba;      /* data base address */
    uint32_t reserved;
    uint32_t dbc;      /* byte count minus one in bits 0-21; bit 31 = interrupt */
} ahci_prdt_entry_t;

/* One command table: the command FIS, the ATAPI command this driver never
 * sends, 48 reserved bytes, and then the PRDT. One entry is enough - the
 * bounce buffer is physically contiguous by construction, so a request is
 * always one region however many sectors it covers. */
typedef struct __attribute__((packed)) {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt[1];
} ahci_cmd_table_t;

/* A host-to-device Register FIS: the 20 bytes that are an ATA command. */
typedef struct __attribute__((packed)) {
    uint8_t fis_type;  /* 0x27 */
    uint8_t pm_flags;  /* bit 7 set = this FIS carries a command, not a control write */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc, control;
    uint8_t reserved[4];
} ahci_h2d_fis_t;

#define FIS_TYPE_REG_H2D 0x27

static volatile uint8_t *abar;
static uint32_t port_num;
static int present;
static uint64_t capacity_sectors;
static uint32_t errors;

static ahci_cmd_header_t *cmd_list;   /* 32 headers, 1 KiB aligned */
static ahci_cmd_table_t *cmd_table;
static uint64_t cmd_list_phys, cmd_table_phys, fis_phys, bounce_phys;
static uint8_t *bounce;

static uint32_t hba_read(uint32_t off) {
    return *(volatile uint32_t *)(abar + off);
}

static void hba_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(abar + off) = val;
}

static uint32_t port_read(uint32_t off) {
    return hba_read(PORT_BASE(port_num) + off);
}

static void port_write(uint32_t off, uint32_t val) {
    hba_write(PORT_BASE(port_num) + off, val);
}

/* ---- Waiting, without a clock ----------------------------------------
 *
 * This runs before the timer subsystem is necessarily useful to a driver
 * and inside a lock the scheduler must not be entered from, so every wait
 * here is a bounded spin rather than a sleep. The bound is a count of
 * iterations rather than a duration, which is deliberately approximate:
 * its only job is to turn "this device never answered" from a hang into a
 * return value. A machine fast enough to burn through the count before a
 * healthy device answers would be a machine where every disk command
 * fails immediately and visibly, which is a bug report rather than a
 * silent corruption. */
#define SPIN_LIMIT 100000000u

static int wait_clear(uint32_t off, uint32_t mask) {
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if ((port_read(off) & mask) == 0) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static void port_stop(void) {
    uint32_t cmd = port_read(PX_CMD);
    port_write(PX_CMD, cmd & ~CMD_ST);
    wait_clear(PX_CMD, CMD_CR);
    cmd = port_read(PX_CMD);
    port_write(PX_CMD, cmd & ~CMD_FRE);
    wait_clear(PX_CMD, CMD_FR);
}

static void port_start(void) {
    /* FRE before ST, and the order is not a style choice: the controller
     * will not leave its idle state with the command list running and no
     * FIS receive area to answer into. */
    wait_clear(PX_CMD, CMD_CR);
    port_write(PX_CMD, port_read(PX_CMD) | CMD_FRE);
    port_write(PX_CMD, port_read(PX_CMD) | CMD_ST);
}

/* Builds one command in slot 0 and waits for it. `write` is the direction
 * from the host's point of view. Returns 0, or -1 on a device error or a
 * command that never completed. */
static int issue_command(uint8_t command, uint64_t lba, uint32_t sectors,
                         uint32_t byte_count, int write) {
    /* A busy port is a port whose last command did not finish. Refusing
     * here rather than queueing behind it keeps the one-command-in-flight
     * invariant this driver's whole design rests on true even when the
     * device has misbehaved. */
    if (port_read(PX_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (wait_clear(PX_TFD, TFD_BSY | TFD_DRQ) != 0) {
            errors++;
            return -1;
        }
    }

    port_write(PX_SERR, port_read(PX_SERR)); /* write-one-to-clear */
    port_write(PX_IS, port_read(PX_IS));

    k_memset(cmd_table, 0, sizeof(*cmd_table));

    ahci_h2d_fis_t *fis = (ahci_h2d_fis_t *)cmd_table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pm_flags = 0x80; /* this FIS is a command */
    fis->command = command;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    /* 0x40 is LBA mode. Bit 4 (drive select) stays clear: there is one
     * device per port on SATA, and the bit that used to choose between
     * master and slave has no meaning here. */
    fis->device = 0x40;
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = (uint8_t)(sectors & 0xFF);
    fis->counth = (uint8_t)((sectors >> 8) & 0xFF);

    if (byte_count > 0) {
        cmd_table->prdt[0].dba = bounce_phys;
        cmd_table->prdt[0].dbc = byte_count - 1; /* a count of zero means one byte */
    }

    cmd_list[0].flags = (uint16_t)((sizeof(ahci_h2d_fis_t) / sizeof(uint32_t)) |
                                   (write ? (1u << 6) : 0));
    cmd_list[0].prdtl = byte_count > 0 ? 1 : 0;
    cmd_list[0].prdbc = 0;
    cmd_list[0].ctba = (uint32_t)(cmd_table_phys & 0xFFFFFFFFu);
    cmd_list[0].ctbau = (uint32_t)(cmd_table_phys >> 32);

    port_write(PX_CI, 1u); /* slot 0 */

    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if ((port_read(PX_CI) & 1u) == 0) {
            break;
        }
        /* A task-file error aborts the command without clearing CI on
         * some controllers, so the error bit has to be checked in the
         * same loop rather than after it. */
        if (port_read(PX_TFD) & TFD_ERR) {
            break;
        }
        __asm__ volatile("pause");
    }

    if ((port_read(PX_CI) & 1u) != 0 || (port_read(PX_TFD) & TFD_ERR)) {
        klog_puts("[ahci] command 0x");
        klog_put_hex32(command);
        klog_puts(" failed: TFD 0x");
        klog_put_hex32(port_read(PX_TFD));
        klog_puts(" SERR 0x");
        klog_put_hex32(port_read(PX_SERR));
        klog_putc('\n');
        port_write(PX_SERR, port_read(PX_SERR));
        errors++;
        return -1;
    }
    return 0;
}

/* IDENTIFY DEVICE, which is how a disk says how big it is. The 512-byte
 * answer comes back through the same PRDT a read does. */
static int identify(void) {
    if (issue_command(ATA_CMD_IDENTIFY, 0, 0, 512, 0) != 0) {
        return -1;
    }
    const uint16_t *id = (const uint16_t *)bounce;
    /* Words 100-103 are the 48-bit sector count. Word 60-61 is the old
     * 28-bit one, used only if the 48-bit field is zero - which it is on
     * a disk old enough not to support LBA48, and this driver only
     * speaks LBA48 commands, so such a disk is declined below. */
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    if (lba48 == 0) {
        klog_puts("[ahci] the disk reports no 48-bit sector count - declining it.\n");
        return -1;
    }
    capacity_sectors = lba48;
    return 0;
}

int ahci_init(void) {
    pci_device_t dev;
    /* Every AHCI controller on the machine, not just the first: a
     * controller with no disk on any port is ordinary, and taking the
     * first one and giving up would be a machine that does not boot for
     * a reason that reads like "no disk". */
    for (uint32_t index = 0; index < 8; index++) {
        if (!pci_find_class(AHCI_CLASS, AHCI_SUBCLASS, AHCI_PROG_IF, index, &dev)) {
            return 0;
        }
        pci_enable_device(&dev);

        /* BAR5 is ABAR - fixed by the specification, not discovered. */
        uint64_t bar = pci_bar_mem_base(&dev, 5);
        uint64_t bar_len = pci_bar_mem_size(&dev, 5);
        if (bar == 0 || bar_len == 0) {
            continue;
        }
        abar = (volatile uint8_t *)vmm_map_mmio(bar, bar_len);
        if (abar == 0) {
            klog_puts("[ahci] ABAR is not mappable - declining this controller.\n");
            continue;
        }

        /* AE before anything else is read: a controller in legacy-IDE
         * emulation answers the port registers with garbage until it is
         * told it is an AHCI controller. */
        hba_write(HBA_GHC, hba_read(HBA_GHC) | GHC_AE);

        uint32_t pi = hba_read(HBA_PI);
        uint32_t cap = hba_read(HBA_CAP);
        int found = -1;
        for (uint32_t p = 0; p < 32; p++) {
            if (!(pi & (1u << p))) {
                continue;
            }
            port_num = p;
            uint32_t ssts = port_read(PX_SSTS);
            /* DET == 3: a device is present and communication is
             * established. IPM == 1: the link is active rather than in a
             * power-saving state this driver would have to wake. */
            if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1) {
                continue;
            }
            if (port_read(PX_SIG) != SIG_SATA) {
                continue; /* ATAPI, or an enclosure service - not a disk */
            }
            found = (int)p;
            break;
        }
        if (found < 0) {
            continue; /* a controller with nothing on it - try the next */
        }
        port_num = (uint32_t)found;

        /* One page holds the command list (1 KiB, and its own alignment
         * requirement is 1 KiB) and the FIS receive area (256 bytes,
         * aligned to 256). A page from pmm_alloc_contiguous is 4 KiB
         * aligned, so putting the list at offset 0 and the FIS area at
         * offset 1024 satisfies both by construction rather than by
         * arithmetic that could drift. */
        uint64_t page = pmm_alloc_contiguous(1);
        k_memset((void *)page, 0, 4096);
        cmd_list_phys = page;
        cmd_list = (ahci_cmd_header_t *)page;
        fis_phys = page + 1024;

        /* The command table wants 128-byte alignment and is bigger than
         * what is left of that page once the FIS area has its 256, so it
         * gets its own. */
        cmd_table_phys = pmm_alloc_contiguous(1);
        cmd_table = (ahci_cmd_table_t *)cmd_table_phys;
        k_memset(cmd_table, 0, 4096);

        bounce_phys = pmm_alloc_contiguous(BOUNCE_BYTES / 4096);
        bounce = (uint8_t *)bounce_phys;

        port_stop();
        port_write(PX_CLB, (uint32_t)(cmd_list_phys & 0xFFFFFFFFu));
        port_write(PX_CLBU, (uint32_t)(cmd_list_phys >> 32));
        port_write(PX_FB, (uint32_t)(fis_phys & 0xFFFFFFFFu));
        port_write(PX_FBU, (uint32_t)(fis_phys >> 32));
        port_write(PX_SERR, port_read(PX_SERR));
        port_write(PX_IS, port_read(PX_IS));
        port_write(PX_IE, 0); /* polled - see the header */
        port_start();

        if (wait_clear(PX_TFD, TFD_BSY | TFD_DRQ) != 0) {
            klog_puts("[ahci] the port never came out of BSY - declining it.\n");
            port_stop();
            continue;
        }

        if (identify() != 0) {
            port_stop();
            continue;
        }

        present = 1;
        klog_puts("[ahci] AHCI ");
        klog_put_hex32(hba_read(HBA_VS));
        klog_puts(", port ");
        klog_put_dec(port_num);
        klog_puts(", ");
        klog_put_dec(capacity_sectors);
        klog_puts(" sectors, ");
        klog_put_dec((cap & 0x1F) + 1);
        klog_puts(" command slots (1 used - see ahci.h)\n");
        return 1;
    }
    return 0;
}

uint64_t ahci_capacity(void) {
    return present ? capacity_sectors : 0;
}

uint32_t ahci_error_count(void) {
    return errors;
}

static int transfer(uint64_t lba, uint32_t count, void *buf, int write) {
    if (!present) {
        return -1;
    }
    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        uint32_t bytes = n * SECTOR_SIZE;
        if (write) {
            k_memcpy(bounce, p, bytes);
        }
        if (issue_command(write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                          lba, n, bytes, write) != 0) {
            return -1;
        }
        if (!write) {
            k_memcpy(p, bounce, bytes);
        }
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

int ahci_read(uint64_t lba, uint32_t count, void *buf) {
    return transfer(lba, count, buf, 0);
}

int ahci_write(uint64_t lba, uint32_t count, const void *buf) {
    return transfer(lba, count, (void *)(uintptr_t)buf, 1);
}
