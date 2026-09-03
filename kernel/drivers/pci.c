#include "pci.h"

#include "arch/x86_64/io.h"
#include "panic.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_REG_VENDOR_DEVICE 0x00
#define PCI_REG_COMMAND       0x04
#define PCI_REG_BAR0          0x10
#define PCI_REG_BAR1          0x14
#define PCI_REG_INTERRUPT     0x3C

static uint32_t pci_config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (offset & 0xFCu);
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_VENDOR_DEVICE);
                uint16_t vid = (uint16_t)(vendor_device & 0xFFFF);
                if (vid == 0xFFFF) {
                    continue; /* no device in this slot/function */
                }
                uint16_t did = (uint16_t)(vendor_device >> 16);
                if (vid == vendor_id && did == device_id) {
                    uint32_t irq = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_INTERRUPT);
                    out->bus = (uint8_t)bus;
                    out->slot = (uint8_t)slot;
                    out->func = (uint8_t)func;
                    out->vendor_id = vid;
                    out->device_id = did;
                    out->irq_line = (uint8_t)(irq & 0xFF);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* ---- Q16: a memory-mapped BAR is a fact about the card ----------------
 *
 * These two halted, and the triage that milestone asks for is what
 * reclassified them: "BAR0 is memory-mapped, not I/O-mapped" is not a
 * kernel bug and it is not an impossible state. It is a perfectly
 * ordinary card that this kernel's three PCI drivers cannot drive,
 * because all three of them speak port I/O - and the same part number
 * genuinely ships both ways. On QEMU's `pc` machine they are always
 * I/O-mapped, which is why nine milestones of PCI work never saw it.
 *
 * 0 means "not an I/O BAR", and 0 is a safe sentinel rather than a
 * convenient one: port 0 is not a device address on x86 and every caller
 * here compares against it before using the value. A driver that gets 0
 * declines the card, and a machine whose sound card is memory-mapped
 * boots to a desktop with no sound instead of not booting. */
uint16_t pci_bar1_io_base(const pci_device_t *dev) {
    uint32_t bar1 = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_REG_BAR1);
    if ((bar1 & 1) == 0) {
        return 0;
    }
    return (uint16_t)(bar1 & 0xFFFC);
}

uint16_t pci_bar0_io_base(const pci_device_t *dev) {
    uint32_t bar0 = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_REG_BAR0);
    if ((bar0 & 1) == 0) {
        return 0;
    }
    return (uint16_t)(bar0 & 0xFFFC);
}

void pci_enable_device(const pci_device_t *dev) {
    uint32_t command = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    command |= (1u << 0); /* I/O Space Enable */
    command |= (1u << 2); /* Bus Master Enable */
    /* M62: and clear Interrupt Disable (bit 10), which UEFI firmware
     * routinely leaves *set* - it has no reason to want INTx while it is
     * driving devices by polling. Nothing here noticed until an AC'97
     * controller became the first device this kernel actually waits on an
     * interrupt from: its completion bits were set in its own status
     * register the whole time and no interrupt had ever been delivered.
     * (rtl8139.c registers a handler too, but polls its status register
     * rather than depending on it, which is why it never found this.) */
    command &= ~(1u << 10);
    pci_config_write32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, command);
}
