#pragma once

#include <stdint.h>

int xhci_init(void);

void xhci_poll(void);

int xhci_device_count(void);

int xhci_storage_present(void);

int xhci_storage_superspeed(void);

int xhci_bulk_in(uint64_t buffer_phys, uint32_t length, uint32_t *transferred_out);

int xhci_bulk_out(uint64_t buffer_phys, uint32_t length, uint32_t *transferred_out);
