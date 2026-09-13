#include "pci.h"

#include "arch/x86_64/io.h"
#include "panic.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_REG_VENDOR_DEVICE 0x00
#define PCI_REG_COMMAND       0x04
#define PCI_REG_STATUS        0x06
#define PCI_REG_REVISION      0x08
#define PCI_REG_HEADER_TYPE   0x0C
#define PCI_REG_BAR0          0x10
#define PCI_REG_BAR1          0x14
#define PCI_REG_CAP_PTR       0x34
#define PCI_REG_INTERRUPT     0x3C

#define PCI_STATUS_CAP_LIST   (1u << 4)
#define PCI_BAR_IO            (1u << 0)
#define PCI_BAR_TYPE_MASK     (3u << 1)
#define PCI_BAR_TYPE_64       (2u << 1)

static uint32_t pci_config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (offset & 0xFCu);
}

static uint32_t cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static void cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint32_t pci_config_read32(const pci_device_t *dev, uint8_t offset) {
    return cfg_read32(dev->bus, dev->slot, dev->func, offset);
}

void pci_config_write32(const pci_device_t *dev, uint8_t offset, uint32_t value) {
    cfg_write32(dev->bus, dev->slot, dev->func, offset, value);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vendor_device = cfg_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_VENDOR_DEVICE);
                uint16_t vid = (uint16_t)(vendor_device & 0xFFFF);
                if (vid == 0xFFFF) {
                    continue;
                }
                uint16_t did = (uint16_t)(vendor_device >> 16);
                if (vid == vendor_id && did == device_id) {
                    uint32_t irq = cfg_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_INTERRUPT);
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

uint16_t pci_bar1_io_base(const pci_device_t *dev) {
    uint32_t bar1 = cfg_read32(dev->bus, dev->slot, dev->func, PCI_REG_BAR1);
    if ((bar1 & 1) == 0) {
        return 0;
    }
    return (uint16_t)(bar1 & 0xFFFC);
}

uint16_t pci_bar0_io_base(const pci_device_t *dev) {
    uint32_t bar0 = cfg_read32(dev->bus, dev->slot, dev->func, PCI_REG_BAR0);
    if ((bar0 & 1) == 0) {
        return 0;
    }
    return (uint16_t)(bar0 & 0xFFFC);
}

void pci_enable_device(const pci_device_t *dev) {
    uint32_t command = cfg_read32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    command |= (1u << 0);
    command |= (1u << 1);
    command |= (1u << 2);
    command &= ~(1u << 10);
    cfg_write32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, command);
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint32_t index, pci_device_t *out) {
    uint32_t seen = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vendor_device =
                    cfg_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_VENDOR_DEVICE);
                uint16_t vid = (uint16_t)(vendor_device & 0xFFFF);
                if (vid == 0xFFFF) {
                    continue;
                }
                uint32_t rev = cfg_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_REVISION);
                uint8_t dev_class = (uint8_t)(rev >> 24);
                uint8_t dev_sub = (uint8_t)(rev >> 16);
                uint8_t dev_prog = (uint8_t)(rev >> 8);
                if (dev_class != class_code || dev_sub != subclass) {
                    continue;
                }
                if (prog_if != PCI_PROG_IF_ANY && dev_prog != prog_if) {
                    continue;
                }
                if (seen++ != index) {
                    continue;
                }
                uint32_t irq = cfg_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_INTERRUPT);
                out->bus = (uint8_t)bus;
                out->slot = (uint8_t)slot;
                out->func = (uint8_t)func;
                out->vendor_id = vid;
                out->device_id = (uint16_t)(vendor_device >> 16);
                out->irq_line = (uint8_t)(irq & 0xFF);
                return 1;
            }
        }
    }
    return 0;
}

static int bar_is_upper_half(const pci_device_t *dev, uint8_t index) {
    if (index == 0) {
        return 0;
    }
    uint32_t below = cfg_read32(dev->bus, dev->slot, dev->func,
                                (uint8_t)(PCI_REG_BAR0 + (index - 1) * 4));
    return !(below & PCI_BAR_IO) && (below & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64;
}

uint64_t pci_bar_mem_base(const pci_device_t *dev, uint8_t index) {
    if (index >= 6 || bar_is_upper_half(dev, index)) {
        return 0;
    }
    uint8_t off = (uint8_t)(PCI_REG_BAR0 + index * 4);
    uint32_t low = cfg_read32(dev->bus, dev->slot, dev->func, off);
    if (low & PCI_BAR_IO) {
        return 0;
    }
    uint64_t base = low & 0xFFFFFFF0u;
    if ((low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) {
        if (index >= 5) {
            return 0;
        }
        base |= (uint64_t)cfg_read32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4)) << 32;
    }
    return base;
}

uint64_t pci_bar_mem_size(const pci_device_t *dev, uint8_t index) {
    if (index >= 6 || bar_is_upper_half(dev, index)) {
        return 0;
    }
    uint8_t off = (uint8_t)(PCI_REG_BAR0 + index * 4);
    uint32_t low = cfg_read32(dev->bus, dev->slot, dev->func, off);
    if (low & PCI_BAR_IO) {
        return 0;
    }
    int is64 = (low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64;
    if (is64 && index >= 5) {
        return 0;
    }

    uint32_t high = is64 ? cfg_read32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4)) : 0;
    cfg_write32(dev->bus, dev->slot, dev->func, off, 0xFFFFFFFFu);
    uint32_t size_low = cfg_read32(dev->bus, dev->slot, dev->func, off);
    uint32_t size_high = 0xFFFFFFFFu;
    if (is64) {
        cfg_write32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4), 0xFFFFFFFFu);
        size_high = cfg_read32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4));
        cfg_write32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4), high);
    }
    cfg_write32(dev->bus, dev->slot, dev->func, off, low);

    uint64_t mask = ((uint64_t)size_high << 32) | (size_low & 0xFFFFFFF0u);
    if (!is64) {
        mask |= 0xFFFFFFFF00000000ULL;
    }
    if (mask == 0xFFFFFFFFFFFFFFF0ULL || mask == 0) {
        return 0;
    }
    return (~mask) + 1;
}

uint8_t pci_find_capability(const pci_device_t *dev, uint8_t cap_id) {
    uint32_t status = cfg_read32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    if (!((status >> 16) & PCI_STATUS_CAP_LIST)) {
        return 0;
    }
    uint8_t off = (uint8_t)(cfg_read32(dev->bus, dev->slot, dev->func, PCI_REG_CAP_PTR) & 0xFC);
    for (int hops = 0; hops < 48 && off >= 0x40; hops++) {
        uint32_t cap = cfg_read32(dev->bus, dev->slot, dev->func, off);
        if ((cap & 0xFF) == cap_id) {
            return off;
        }
        off = (uint8_t)((cap >> 8) & 0xFC);
    }
    return 0;
}
