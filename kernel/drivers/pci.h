/* kernel/drivers/pci.h
 *
 * PCI config-space access, just enough to find a device by vendor/device
 * ID and read what a driver needs to talk to it: its I/O-space BAR and
 * the legacy IRQ line firmware routed it to. Full bus enumeration (every
 * bus/slot/function, brute-force) rather than walking bridges properly -
 * this kernel has exactly one PCI device it's ever looked for (the
 * networking stretch goal's NIC), so there's no capability list, MSI, or
 * memory-space BAR handling here; add it if a second device ever needs it.
 */
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

/* Scans every bus/slot/function for a matching vendor+device ID pair,
 * filling *out and returning 1 on the first match, 0 if none exists. */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

/* Reads BAR0 and masks off the low flag bits, returning the I/O port base
 * it encodes. Panics if BAR0 turns out to be memory-mapped (bit 0 clear) -
 * every device this driver looks for today is I/O-mapped; a memory-BAR
 * device needs vmm mapping support this function doesn't attempt. */
/* Q16: 0 if the BAR is memory-mapped rather than I/O-mapped, which is a
 * fact about the card rather than a bug in this kernel - and used to
 * halt the machine. Every driver here speaks port I/O, so 0 means
 * "decline this card"; port 0 is not a device address on x86, which is
 * what makes it a safe sentinel rather than a convenient one. */
uint16_t pci_bar0_io_base(const pci_device_t *dev);

/* M62: the same for BAR1, which is where a device's *second* I/O region
 * lives - the AC'97 controller has two (a mixer and a bus-master block)
 * and needs both. Split from pci_bar0_io_base rather than parameterised,
 * because "which BAR" is a fact about a device rather than a loop
 * variable, and two named functions read better at the two call sites
 * than one that takes an index. */
uint16_t pci_bar1_io_base(const pci_device_t *dev);

/* Sets the Bus Master and I/O Space enable bits in the command register -
 * required before a device can do DMA (bus mastering) or respond to I/O
 * port cycles at all. */
void pci_enable_device(const pci_device_t *dev);
