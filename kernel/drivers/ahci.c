#include "ahci.h"

#include "drivers/kernel_log.h"
#include "drivers/pci.h"
#include "library/kernel_library.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"

#define AHCI_CLASS    0x01
#define AHCI_SUBCLASS 0x06
#define AHCI_PROG_IF  0x01

#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_IS   0x08
#define HBA_PI   0x0C
#define HBA_VS   0x10

#define GHC_HR   (1u << 0)
#define GHC_IE   (1u << 1)
#define GHC_AE   (1u << 31)

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

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)

#define TFD_ERR  (1u << 0)
#define TFD_DRQ  (1u << 3)
#define TFD_BSY  (1u << 7)

#define SIG_SATA 0x00000101u

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC

#define SECTOR_SIZE 512

#define BOUNCE_SECTORS 128
#define BOUNCE_BYTES   (BOUNCE_SECTORS * SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_command_header_t;

typedef struct __attribute__((packed)) {
    uint64_t dba;
    uint32_t reserved;
    uint32_t dbc;
} ahci_prdt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt[1];
} ahci_command_table_t;

typedef struct __attribute__((packed)) {
    uint8_t fis_type;
    uint8_t pm_flags;
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
static uint32_t port_number;
static int present;
static uint64_t capacity_sectors;
static uint32_t errors;

static ahci_command_header_t *command_list;
static ahci_command_table_t *command_table;
static uint64_t command_list_phys, command_table_phys, fis_phys, bounce_phys;
static uint8_t *bounce;

static uint32_t hba_read(uint32_t off) {
    return *(volatile uint32_t *)(abar + off);
}

static void hba_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(abar + off) = val;
}

static uint32_t port_read(uint32_t off) {
    return hba_read(PORT_BASE(port_number) + off);
}

static void port_write(uint32_t off, uint32_t val) {
    hba_write(PORT_BASE(port_number) + off, val);
}

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
    wait_clear(PX_CMD, CMD_CR);
    port_write(PX_CMD, port_read(PX_CMD) | CMD_FRE);
    port_write(PX_CMD, port_read(PX_CMD) | CMD_ST);
}

static int issue_command(uint8_t command, uint64_t lba, uint32_t sectors,
                         uint32_t byte_count, int write) {
    if (port_read(PX_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (wait_clear(PX_TFD, TFD_BSY | TFD_DRQ) != 0) {
            errors++;
            return -1;
        }
    }

    port_write(PX_SERR, port_read(PX_SERR));
    port_write(PX_IS, port_read(PX_IS));

    k_memset(command_table, 0, sizeof(*command_table));

    ahci_h2d_fis_t *fis = (ahci_h2d_fis_t *)command_table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pm_flags = 0x80;
    fis->command = command;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 0x40;
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = (uint8_t)(sectors & 0xFF);
    fis->counth = (uint8_t)((sectors >> 8) & 0xFF);

    if (byte_count > 0) {
        command_table->prdt[0].dba = bounce_phys;
        command_table->prdt[0].dbc = byte_count - 1;
    }

    command_list[0].flags = (uint16_t)((sizeof(ahci_h2d_fis_t) / sizeof(uint32_t)) |
                                   (write ? (1u << 6) : 0));
    command_list[0].prdtl = byte_count > 0 ? 1 : 0;
    command_list[0].prdbc = 0;
    command_list[0].ctba = (uint32_t)(command_table_phys & 0xFFFFFFFFu);
    command_list[0].ctbau = (uint32_t)(command_table_phys >> 32);

    port_write(PX_CI, 1u);

    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if ((port_read(PX_CI) & 1u) == 0) {
            break;
        }
        if (port_read(PX_TFD) & TFD_ERR) {
            break;
        }
        __asm__ volatile("pause");
    }

    if ((port_read(PX_CI) & 1u) != 0 || (port_read(PX_TFD) & TFD_ERR)) {
        kernel_log_puts("[ahci] command 0x");
        kernel_log_put_hex32(command);
        kernel_log_puts(" failed: TFD 0x");
        kernel_log_put_hex32(port_read(PX_TFD));
        kernel_log_puts(" SERR 0x");
        kernel_log_put_hex32(port_read(PX_SERR));
        kernel_log_putc('\n');
        port_write(PX_SERR, port_read(PX_SERR));
        errors++;
        return -1;
    }
    return 0;
}

static int identify(void) {
    if (issue_command(ATA_CMD_IDENTIFY, 0, 0, 512, 0) != 0) {
        return -1;
    }
    const uint16_t *id = (const uint16_t *)bounce;
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    if (lba48 == 0) {
        kernel_log_puts("[ahci] the disk reports no 48-bit sector count - declining it.\n");
        return -1;
    }
    capacity_sectors = lba48;
    return 0;
}

int ahci_init(void) {
    pci_device_t dev;
    for (uint32_t index = 0; index < 8; index++) {
        if (!pci_find_class(AHCI_CLASS, AHCI_SUBCLASS, AHCI_PROG_IF, index, &dev)) {
            return 0;
        }
        pci_enable_device(&dev);

        uint64_t bar = pci_bar_memory_base(&dev, 5);
        uint64_t bar_length = pci_bar_memory_size(&dev, 5);
        if (bar == 0 || bar_length == 0) {
            continue;
        }
        abar = (volatile uint8_t *)virtual_memory_map_mmio(bar, bar_length);
        if (abar == 0) {
            kernel_log_puts("[ahci] ABAR is not mappable - declining this controller.\n");
            continue;
        }

        hba_write(HBA_GHC, hba_read(HBA_GHC) | GHC_AE);

        uint32_t pi = hba_read(HBA_PI);
        uint32_t cap = hba_read(HBA_CAP);
        int found = -1;
        for (uint32_t p = 0; p < 32; p++) {
            if (!(pi & (1u << p))) {
                continue;
            }
            port_number = p;
            uint32_t ssts = port_read(PX_SSTS);
            if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1) {
                continue;
            }
            if (port_read(PX_SIG) != SIG_SATA) {
                continue;
            }
            found = (int)p;
            break;
        }
        if (found < 0) {
            continue;
        }
        port_number = (uint32_t)found;

        uint64_t page = physical_memory_alloc_contiguous(1);
        k_memset((void *)page, 0, 4096);
        command_list_phys = page;
        command_list = (ahci_command_header_t *)page;
        fis_phys = page + 1024;

        command_table_phys = physical_memory_alloc_contiguous(1);
        command_table = (ahci_command_table_t *)command_table_phys;
        k_memset(command_table, 0, 4096);

        bounce_phys = physical_memory_alloc_contiguous(BOUNCE_BYTES / 4096);
        bounce = (uint8_t *)bounce_phys;

        port_stop();
        port_write(PX_CLB, (uint32_t)(command_list_phys & 0xFFFFFFFFu));
        port_write(PX_CLBU, (uint32_t)(command_list_phys >> 32));
        port_write(PX_FB, (uint32_t)(fis_phys & 0xFFFFFFFFu));
        port_write(PX_FBU, (uint32_t)(fis_phys >> 32));
        port_write(PX_SERR, port_read(PX_SERR));
        port_write(PX_IS, port_read(PX_IS));
        port_write(PX_IE, 0);
        port_start();

        if (wait_clear(PX_TFD, TFD_BSY | TFD_DRQ) != 0) {
            kernel_log_puts("[ahci] the port never came out of BSY - declining it.\n");
            port_stop();
            continue;
        }

        if (identify() != 0) {
            port_stop();
            continue;
        }

        present = 1;
        kernel_log_puts("[ahci] AHCI ");
        kernel_log_put_hex32(hba_read(HBA_VS));
        kernel_log_puts(", port ");
        kernel_log_put_dec(port_number);
        kernel_log_puts(", ");
        kernel_log_put_dec(capacity_sectors);
        kernel_log_puts(" sectors, ");
        kernel_log_put_dec((cap & 0x1F) + 1);
        kernel_log_puts(" command slots (1 used - see ahci.h)\n");
        return 1;
    }
    return 0;
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
