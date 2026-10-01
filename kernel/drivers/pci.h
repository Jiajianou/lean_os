#pragma once

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t irq_line;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
} pci_device_t;

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

#define PCI_PROG_IF_ANY 0xFF
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint32_t index, pci_device_t *out);

typedef void (*pci_visitor_t)(const pci_device_t *device, void *context);

void pci_enumerate(pci_visitor_t visit, void *context);

void pci_rescan(void);

uint16_t pci_bar0_io_base(const pci_device_t *dev);

uint16_t pci_bar1_io_base(const pci_device_t *dev);

void pci_enable_device(const pci_device_t *dev);

void pci_disable_legacy_interrupt(const pci_device_t *dev);

uint64_t pci_bar_memory_base(const pci_device_t *dev, uint8_t index);

uint64_t pci_bar_memory_size(const pci_device_t *dev, uint8_t index);

uint64_t pci_assign_memory_bar(const pci_device_t *dev, uint8_t index, uint64_t address_limit);

#define PCI_CAP_ID_MSI  0x05
#define PCI_CAP_ID_MSIX 0x11
#define PCI_CAP_ID_POWER_MANAGEMENT 0x01
uint8_t pci_find_capability(const pci_device_t *dev, uint8_t cap_id);

void pci_set_power_state_d0(const pci_device_t *dev);

