#pragma once

#include <stdint.h>

int xhci_init(void);

void xhci_poll(void);

int xhci_device_count(void);

uint64_t xhci_keyboard_reports(void);
uint64_t xhci_mouse_reports(void);
