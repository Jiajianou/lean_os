#include "xhci_ring.h"

void xhci_ring_reset(xhci_ring_t *r, void *buffer, uint64_t phys) {
    xhci_trb_t *trb = (xhci_trb_t *)buffer;
    for (uint32_t i = 0; i < XHCI_RING_TRBS; i++) {
        trb[i].parameter = 0;
        trb[i].status = 0;
        trb[i].control = 0;
    }
    r->trb = trb;
    r->phys = phys;
    r->index = 0;
    r->cycle = 1;
    trb[XHCI_RING_TRBS - 1].parameter = phys;
    trb[XHCI_RING_TRBS - 1].status = 0;
    trb[XHCI_RING_TRBS - 1].control =
        (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | XHCI_TRB_CYCLE;
}

void xhci_ring_push(xhci_ring_t *r, uint64_t parameter, uint32_t status, uint32_t control) {
    xhci_trb_t *t = &r->trb[r->index];
    t->parameter = parameter;
    t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = control | (r->cycle ? XHCI_TRB_CYCLE : 0);

    r->index++;
    if (r->index == XHCI_RING_TRBS - 1) {
        xhci_trb_t *link = &r->trb[XHCI_RING_TRBS - 1];
        link->control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC |
                        (r->cycle ? XHCI_TRB_CYCLE : 0);
        r->index = 0;
        r->cycle ^= 1;
    }
}
