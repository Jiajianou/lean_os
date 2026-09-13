#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

#define XHCI_RING_TRBS 16

#define XHCI_TRB_CYCLE      (1u << 0)
#define XHCI_TRB_TC         (1u << 1)
#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_LINK       6

typedef struct {
    xhci_trb_t *trb;
    uint64_t phys;
    uint32_t index;
    uint8_t cycle;
} xhci_ring_t;

void xhci_ring_reset(xhci_ring_t *r, void *buf, uint64_t phys);

void xhci_ring_push(xhci_ring_t *r, uint64_t param, uint32_t status, uint32_t control);
