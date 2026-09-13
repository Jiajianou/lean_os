#pragma once

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t irq_line;
} pci_device_t;

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

#define PCI_PROG_IF_ANY 0xFF
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint32_t index, pci_device_t *out);

uint16_t pci_bar0_io_base(const pci_device_t *dev);

uint16_t pci_bar1_io_base(const pci_device_t *dev);

void pci_enable_device(const pci_device_t *dev);

uint64_t pci_bar_mem_base(const pci_device_t *dev, uint8_t index);

uint64_t pci_bar_mem_size(const pci_device_t *dev, uint8_t index);

#define PCI_CAP_ID_MSI  0x05
#define PCI_CAP_ID_MSIX 0x11
uint8_t pci_find_capability(const pci_device_t *dev, uint8_t cap_id);

