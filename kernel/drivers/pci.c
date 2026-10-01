#include "pci.h"

#include "architecture/x86_64/io.h"
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
#define PCI_REG_CAP_POINTER       0x34
#define PCI_REG_INTERRUPT     0x3C

#define PCI_STATUS_CAP_LIST   (1u << 4)
#define PCI_BAR_IO            (1u << 0)
#define PCI_BAR_TYPE_MASK     (3u << 1)
#define PCI_BAR_TYPE_64       (2u << 1)

static uint32_t pci_config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (offset & 0xFCu);
}

static uint32_t config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static void config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static void fill(pci_device_t *out, uint8_t bus, uint8_t slot, uint8_t func,
                 uint16_t vendor_id, uint16_t device_id) {
    uint32_t rev = config_read32(bus, slot, func, PCI_REG_REVISION);
    uint32_t irq = config_read32(bus, slot, func, PCI_REG_INTERRUPT);
    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor_id = vendor_id;
    out->device_id = device_id;
    out->irq_line = (uint8_t)(irq & 0xFF);
    out->class_code = (uint8_t)(rev >> 24);
    out->subclass = (uint8_t)(rev >> 16);
    out->prog_if = (uint8_t)(rev >> 8);
}

/* M205. Every probe used to walk all 65,536 bus/slot/function triples, two
   port accesses each, and a driver looking for something absent walked them
   all. On the laptop's chipset that is half a second a walk, and booting made
   eight of them before the desktop. A function other than 0 is only looked
   for where function 0 answered, which the specification requires of every
   device, and the machine is walked once. */
#define PCI_MAX_FUNCTIONS 256

static pci_device_t found_functions[PCI_MAX_FUNCTIONS];
static uint32_t found_count;
static int scanned;

static void scan(void) {
    found_count = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vendor_device =
                    config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_REG_VENDOR_DEVICE);
                uint16_t vid = (uint16_t)(vendor_device & 0xFFFF);
                if (vid == 0xFFFF) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }
                if (found_count == PCI_MAX_FUNCTIONS) {
                    return;
                }
                fill(&found_functions[found_count++], (uint8_t)bus, (uint8_t)slot, (uint8_t)func, vid,
                     (uint16_t)(vendor_device >> 16));
            }
        }
    }
}

static void ensure_scanned(void) {
    if (!scanned) {
        scan();
        scanned = 1;
    }
}

void pci_rescan(void) {
    scanned = 0;
}

void pci_enumerate(pci_visitor_t visit, void *context) {
    ensure_scanned();
    for (uint32_t i = 0; i < found_count; i++) {
        pci_device_t device = found_functions[i];
        visit(&device, context);
    }
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out) {
    ensure_scanned();
    for (uint32_t i = 0; i < found_count; i++) {
        if (found_functions[i].vendor_id == vendor_id && found_functions[i].device_id == device_id) {
            *out = found_functions[i];
            return 1;
        }
    }
    return 0;
}

uint16_t pci_bar1_io_base(const pci_device_t *dev) {
    uint32_t bar1 = config_read32(dev->bus, dev->slot, dev->func, PCI_REG_BAR1);
    if ((bar1 & 1) == 0) {
        return 0;
    }
    return (uint16_t)(bar1 & 0xFFFC);
}

uint16_t pci_bar0_io_base(const pci_device_t *dev) {
    uint32_t bar0 = config_read32(dev->bus, dev->slot, dev->func, PCI_REG_BAR0);
    if ((bar0 & 1) == 0) {
        return 0;
    }
    return (uint16_t)(bar0 & 0xFFFC);
}

void pci_enable_device(const pci_device_t *dev) {
    uint32_t command = config_read32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    command |= (1u << 0);
    command |= (1u << 1);
    command |= (1u << 2);
    command &= ~(1u << 10);
    config_write32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, command);
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint32_t index, pci_device_t *out) {
    ensure_scanned();
    uint32_t seen = 0;
    for (uint32_t i = 0; i < found_count; i++) {
        const pci_device_t *device = &found_functions[i];
        if (device->class_code != class_code || device->subclass != subclass) {
            continue;
        }
        if (prog_if != PCI_PROG_IF_ANY && device->prog_if != prog_if) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        *out = *device;
        return 1;
    }
    return 0;
}

static int bar_is_upper_half(const pci_device_t *dev, uint8_t index) {
    if (index == 0) {
        return 0;
    }
    uint32_t below = config_read32(dev->bus, dev->slot, dev->func,
                                (uint8_t)(PCI_REG_BAR0 + (index - 1) * 4));
    return !(below & PCI_BAR_IO) && (below & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64;
}

uint64_t pci_bar_memory_base(const pci_device_t *dev, uint8_t index) {
    if (index >= 6 || bar_is_upper_half(dev, index)) {
        return 0;
    }
    uint8_t off = (uint8_t)(PCI_REG_BAR0 + index * 4);
    uint32_t low = config_read32(dev->bus, dev->slot, dev->func, off);
    if (low & PCI_BAR_IO) {
        return 0;
    }
    uint64_t base = low & 0xFFFFFFF0u;
    if ((low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) {
        if (index >= 5) {
            return 0;
        }
        base |= (uint64_t)config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4)) << 32;
    }
    return base;
}

uint64_t pci_bar_memory_size(const pci_device_t *dev, uint8_t index) {
    if (index >= 6 || bar_is_upper_half(dev, index)) {
        return 0;
    }
    uint8_t off = (uint8_t)(PCI_REG_BAR0 + index * 4);
    uint32_t low = config_read32(dev->bus, dev->slot, dev->func, off);
    if (low & PCI_BAR_IO) {
        return 0;
    }
    int is64 = (low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64;
    if (is64 && index >= 5) {
        return 0;
    }

    uint32_t high = is64 ? config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4)) : 0;
    config_write32(dev->bus, dev->slot, dev->func, off, 0xFFFFFFFFu);
    uint32_t size_low = config_read32(dev->bus, dev->slot, dev->func, off);
    uint32_t size_high = 0xFFFFFFFFu;
    if (is64) {
        config_write32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4), 0xFFFFFFFFu);
        size_high = config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4));
        config_write32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4), high);
    }
    config_write32(dev->bus, dev->slot, dev->func, off, low);

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
    uint32_t status = config_read32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    if (!((status >> 16) & PCI_STATUS_CAP_LIST)) {
        return 0;
    }
    uint8_t off = (uint8_t)(config_read32(dev->bus, dev->slot, dev->func, PCI_REG_CAP_POINTER) & 0xFC);
    for (int hops = 0; hops < 48 && off >= 0x40; hops++) {
        uint32_t cap = config_read32(dev->bus, dev->slot, dev->func, off);
        if ((cap & 0xFF) == cap_id) {
            return off;
        }
        off = (uint8_t)((cap >> 8) & 0xFC);
    }
    return 0;
}

void pci_set_power_state_d0(const pci_device_t *dev) {
    uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_POWER_MANAGEMENT);
    if (cap == 0) {
        return;
    }
    uint32_t control = config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(cap + 4));
    if ((control & 0x3u) == 0) {
        return;
    }
    control &= ~0x3u;
    config_write32(dev->bus, dev->slot, dev->func, (uint8_t)(cap + 4), control);
    for (volatile int settle = 0; settle < 100000; settle++) {
    }
}

typedef struct {
    uint64_t highest_end;
} occupied_t;

static uint64_t window_end(uint32_t limit_bits, uint32_t upper) {
    return ((((uint64_t)upper << 32) | ((uint64_t)(limit_bits & 0xFFF0u) << 16)) | 0xFFFFFu) + 1;
}

static void note_occupied(const pci_device_t *device, void *context) {
    occupied_t *occupied = (occupied_t *)context;
    uint32_t header = (config_read32(device->bus, device->slot, device->func, PCI_REG_HEADER_TYPE) >> 16) & 0x7F;
    uint8_t bars = header == 1 ? 2 : (header == 0 ? 6 : 0);

    uint32_t command = config_read32(device->bus, device->slot, device->func, PCI_REG_COMMAND);
    config_write32(device->bus, device->slot, device->func, PCI_REG_COMMAND, command & ~(1u << 1));
    for (uint8_t index = 0; index < bars; index++) {
        uint64_t base = pci_bar_memory_base(device, index);
        if (base == 0) {
            continue;
        }
        uint64_t end = base + pci_bar_memory_size(device, index);
        if (end > occupied->highest_end) {
            occupied->highest_end = end;
        }
    }
    config_write32(device->bus, device->slot, device->func, PCI_REG_COMMAND, command);

    if (header == 1) {
        uint32_t memory = config_read32(device->bus, device->slot, device->func, 0x20);
        uint64_t memory_base = (uint64_t)(memory & 0xFFF0u) << 16;
        uint64_t memory_end = window_end(memory >> 16, 0);
        if (memory_end - 1 >= memory_base && memory_end > occupied->highest_end) {
            occupied->highest_end = memory_end;
        }
        uint32_t prefetchable = config_read32(device->bus, device->slot, device->func, 0x24);
        uint32_t base_upper = config_read32(device->bus, device->slot, device->func, 0x28);
        uint32_t limit_upper = config_read32(device->bus, device->slot, device->func, 0x2C);
        uint64_t prefetchable_base = ((uint64_t)base_upper << 32) | ((uint64_t)(prefetchable & 0xFFF0u) << 16);
        uint64_t prefetchable_end = window_end(prefetchable >> 16, limit_upper);
        if (prefetchable_end - 1 >= prefetchable_base && prefetchable_end > occupied->highest_end) {
            occupied->highest_end = prefetchable_end;
        }
    }
}

uint64_t pci_assign_memory_bar(const pci_device_t *dev, uint8_t index, uint64_t address_limit) {
    uint64_t existing = pci_bar_memory_base(dev, index);
    if (existing != 0) {
        return existing;
    }
    uint8_t off = (uint8_t)(PCI_REG_BAR0 + index * 4);
    uint32_t low = config_read32(dev->bus, dev->slot, dev->func, off);
    if ((low & PCI_BAR_IO) || (low & PCI_BAR_TYPE_MASK) != PCI_BAR_TYPE_64) {
        return 0;
    }
    uint64_t size = pci_bar_memory_size(dev, index);
    if (size == 0) {
        return 0;
    }

    occupied_t occupied = {0};
    pci_enumerate(note_occupied, &occupied);
    if (occupied.highest_end <= 0x100000000ULL) {
        return 0;
    }
    uint64_t alignment = size < 0x100000 ? 0x100000 : size;
    uint64_t place = (occupied.highest_end + alignment - 1) & ~(alignment - 1);
    if (place + size > address_limit || place + size < place) {
        return 0;
    }

    config_write32(dev->bus, dev->slot, dev->func, off, (uint32_t)place | (low & 0x0Fu));
    config_write32(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4), (uint32_t)(place >> 32));
    return pci_bar_memory_base(dev, index);
}
