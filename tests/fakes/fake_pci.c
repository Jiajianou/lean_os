/* tests/fakes/fake_pci.c - M107
 *
 * A PCI configuration space, on the two dword ports that are the whole
 * mechanism.
 *
 * The same move Q11 made for the byte ports and kernel/dev/fwcfg.c: the
 * dword ports used to read back 0xFFFFFFFF unconditionally, which is what
 * an unclaimed port does and is exactly what "there is no device here"
 * looks like - so pci.c compiled into this tier would find nothing, every
 * time, and prove nothing.
 *
 * What makes this worth modelling rather than mocking pci.c's callers:
 * BAR decoding is arithmetic with sharp edges, and every edge is a real
 * machine. A 64-bit BAR is two registers that must be joined and whose
 * upper half must not be mistaken for a BAR of its own. The size probe
 * writes all ones and must put the original value back, because a BAR
 * left holding 0xFFFFFFFF is a device answering at an address nothing
 * else knows about. And a device that is not there must decode to zero
 * rather than to 0xFFFFFFF0, which is what the unmodelled port returned.
 *
 * None of that is reachable from a booted machine: QEMU's `pc` machine
 * hands out one layout, and the first time a 64-bit BAR above 4 GiB
 * appeared - the NVMe controller, in this milestone - it page-faulted the
 * kernel. That bug was in the mapping rather than in the decode, and it
 * is exactly why the decode is worth a test of its own.
 */
#include "fakes.h"

#include <string.h>

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

#define MAX_DEVICES 8

typedef struct {
    int present;
    uint8_t bus, slot, func;
    uint32_t config[64]; /* 256 bytes of config space, as dwords */
} fake_pci_device_t;

static fake_pci_device_t devices[MAX_DEVICES];
static uint32_t selected; /* the last value written to CONFIG_ADDRESS */

void fake_pci_reset(void) {
    memset(devices, 0, sizeof(devices));
    selected = 0;
}

static fake_pci_device_t *find(uint8_t bus, uint8_t slot, uint8_t func) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].present && devices[i].bus == bus && devices[i].slot == slot &&
            devices[i].func == func) {
            return &devices[i];
        }
    }
    return 0;
}

int fake_pci_add(uint8_t bus, uint8_t slot, uint8_t func, uint16_t vendor, uint16_t device,
                 uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].present) {
            continue;
        }
        memset(&devices[i], 0, sizeof(devices[i]));
        devices[i].present = 1;
        devices[i].bus = bus;
        devices[i].slot = slot;
        devices[i].func = func;
        devices[i].config[0] = ((uint32_t)device << 16) | vendor;
        devices[i].config[2] = ((uint32_t)class_code << 24) | ((uint32_t)subclass << 16) |
                               ((uint32_t)prog_if << 8);
        return i;
    }
    return -1;
}

void fake_pci_set_config(int handle, uint8_t offset, uint32_t value) {
    if (handle < 0 || handle >= MAX_DEVICES) {
        return;
    }
    devices[handle].config[offset / 4] = value;
}

uint32_t fake_pci_get_config(int handle, uint8_t offset) {
    if (handle < 0 || handle >= MAX_DEVICES) {
        return 0;
    }
    return devices[handle].config[offset / 4];
}

/* A BAR's writable bits are the ones above its size: writing all ones and
 * reading back is how a driver discovers the size, and what comes back is
 * the size mask with the low flag bits preserved. That is modelled here
 * rather than assumed, because it is the behaviour the code under test is
 * written against. */
static uint32_t bar_mask[MAX_DEVICES][6];
static int bar_is_bar[MAX_DEVICES][6];

void fake_pci_set_bar(int handle, int index, uint32_t value, uint32_t size_mask) {
    if (handle < 0 || handle >= MAX_DEVICES || index < 0 || index >= 6) {
        return;
    }
    devices[handle].config[(0x10 + index * 4) / 4] = value;
    bar_mask[handle][index] = size_mask;
    bar_is_bar[handle][index] = 1;
}

static int device_index(const fake_pci_device_t *d) {
    return (int)(d - devices);
}

uint32_t fake_port_inl(uint16_t port) {
    if (port != CONFIG_DATA || !(selected & (1u << 31))) {
        return 0xFFFFFFFFu;
    }
    uint8_t bus = (uint8_t)((selected >> 16) & 0xFF);
    uint8_t slot = (uint8_t)((selected >> 11) & 0x1F);
    uint8_t func = (uint8_t)((selected >> 8) & 0x07);
    uint8_t offset = (uint8_t)(selected & 0xFC);

    fake_pci_device_t *d = find(bus, slot, func);
    if (!d) {
        return 0xFFFFFFFFu; /* no device: the vendor id reads back all ones */
    }
    return d->config[offset / 4];
}

void fake_port_outl(uint16_t port, uint32_t value) {
    if (port == CONFIG_ADDRESS) {
        selected = value;
        return;
    }
    if (port != CONFIG_DATA || !(selected & (1u << 31))) {
        return;
    }
    uint8_t bus = (uint8_t)((selected >> 16) & 0xFF);
    uint8_t slot = (uint8_t)((selected >> 11) & 0x1F);
    uint8_t func = (uint8_t)((selected >> 8) & 0x07);
    uint8_t offset = (uint8_t)(selected & 0xFC);

    fake_pci_device_t *d = find(bus, slot, func);
    if (!d) {
        return;
    }
    int idx = device_index(d);
    if (offset >= 0x10 && offset < 0x28) {
        int bar = (offset - 0x10) / 4;
        if (bar_is_bar[idx][bar] && value == 0xFFFFFFFFu) {
            /* The size probe. What comes back is the size mask with the
             * BAR's own type bits kept, which is what real hardware does
             * and what pci_bar_mem_size has to cope with. */
            uint32_t type_bits = d->config[offset / 4] & 0x0F;
            d->config[offset / 4] = (bar_mask[idx][bar] & ~0x0Fu) | type_bits;
            return;
        }
    }
    d->config[offset / 4] = value;
}
