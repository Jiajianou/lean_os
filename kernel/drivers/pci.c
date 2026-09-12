#include "pci.h"

#include "arch/x86_64/io.h"
#include "panic.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_REG_VENDOR_DEVICE 0x00
#define PCI_REG_COMMAND       0x04
#define PCI_REG_STATUS        0x06
#define PCI_REG_REVISION      0x08 /* prog-if, subclass, class in the upper three bytes */
#define PCI_REG_HEADER_TYPE   0x0C /* byte 2: bit 7 = multifunction, low bits = header layout */
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
                    continue; /* no device in this slot/function */
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
    command |= (1u << 0); /* I/O Space Enable */
    /* M107: Memory Space Enable, which no driver here needed until three
     * arrived whose registers are only reachable that way. Setting it on
     * a device whose BARs are all I/O costs nothing - firmware has
     * already assigned the BARs either way, and this bit only says
     * whether the device answers cycles in that space. */
    command |= (1u << 1); /* Memory Space Enable */
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
    cfg_write32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, command);
}

/* ---- M107: finding a device by what it IS rather than by who made it --
 *
 * pci_find_device above matches a vendor and device id, which is the
 * right question for virtio-blk, the RTL8139 and the AC'97 controller:
 * each of those drivers speaks one manufacturer's register layout and
 * would be wrong to bind to anything else.
 *
 * The three controllers M107 adds are the opposite case. "AHCI" is a
 * register interface standardised across every manufacturer, and the
 * class code is the field that says so. Matching by vendor id would mean
 * a table of every SATA controller ever shipped, and would still miss the
 * one in the machine somebody actually has. */
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

/* Is BAR `index` the upper half of the 64-bit BAR below it? A caller that
 * asks about BAR1 of a device whose BAR0 is 64-bit is asking about half
 * an address, and the honest answer is "there is no BAR there". */
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
        return 0; /* an I/O BAR - pci_bar0_io_base is the function for that */
    }
    uint64_t base = low & 0xFFFFFFF0u;
    if ((low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) {
        if (index >= 5) {
            return 0; /* a 64-bit BAR with no register to hold its top half */
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

    /* The sizing protocol: all ones in, and the bits that read back zero
     * are the ones the device does not decode. The device stops
     * responding at its old address while the probe value is in there,
     * which is why this runs at boot with nothing using the BAR - and why
     * the original value goes back before anything else happens. */
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
        mask |= 0xFFFFFFFF00000000ULL; /* only the low 32 bits were probed */
    }
    if (mask == 0xFFFFFFFFFFFFFFF0ULL || mask == 0) {
        return 0; /* unimplemented BAR */
    }
    return (~mask) + 1;
}

uint8_t pci_find_capability(const pci_device_t *dev, uint8_t cap_id) {
    uint32_t status = cfg_read32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    if (!((status >> 16) & PCI_STATUS_CAP_LIST)) {
        return 0;
    }
    uint8_t off = (uint8_t)(cfg_read32(dev->bus, dev->slot, dev->func, PCI_REG_CAP_PTR) & 0xFC);
    /* 48 hops is more than the 256-byte config space can hold distinct
     * 4-byte capabilities, so a list that has not ended by then is a loop
     * - which is a thing firmware does, and which must not hang a boot. */
    for (int hops = 0; hops < 48 && off >= 0x40; hops++) {
        uint32_t cap = cfg_read32(dev->bus, dev->slot, dev->func, off);
        if ((cap & 0xFF) == cap_id) {
            return off;
        }
        off = (uint8_t)((cap >> 8) & 0xFC);
    }
    return 0;
}
