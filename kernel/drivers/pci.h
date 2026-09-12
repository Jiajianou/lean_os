/* kernel/drivers/pci.h
 *
 * PCI config-space access: find a device, and read what a driver needs to
 * talk to it.
 *
 * ---- M107 took the "add it if a second device ever needs it" bet ------
 *
 * The header used to end by saying there was no capability list, no MSI
 * and no memory-space BAR handling here, and to add them if a second
 * device ever needed them. Three did, at once, and they are the three a
 * machine built after about 2005 actually has: AHCI, NVMe and xHCI. Every
 * one of them is found by *class* rather than by vendor and device id -
 * an AHCI controller is class 01:06:01 whoever made it, which is the
 * whole reason one driver can drive Intel's, AMD's and QEMU's - and every
 * one of them puts its registers behind a memory BAR rather than an I/O
 * one, because none of the three has a port-I/O interface at all.
 *
 * So what is here now: a class scan with an iterator (a machine can have
 * two AHCI controllers and this kernel wants the one with a disk on it),
 * 32- and 64-bit memory BAR decoding, and the capability list - which is
 * how a driver asks "does this device speak MSI-X" without guessing.
 *
 * Still brute-force over every bus/slot/function rather than walking
 * bridges properly. That is unchanged and is still right: 256 buses x 32
 * slots x 8 functions is 65,536 config reads, it happens twice at boot,
 * and a bridge walk would be more code that finds the same devices.
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

/* M107: the same scan, by class/subclass/programming interface.
 *
 * `prog_if` may be PCI_PROG_IF_ANY, which is the difference between "an
 * NVMe controller" (01:08:02, a specific programming interface) and "a
 * USB controller of any kind" (0C:03:xx, where the prog-if is which of
 * UHCI/OHCI/EHCI/xHCI it is and a driver for one must NOT match the
 * others). `index` skips that many matches first, so a caller can walk
 * every controller of a class rather than take the first and hope: a
 * machine with two AHCI controllers, one of them empty, is ordinary.
 *
 * Returns 1 and fills *out, or 0 when there is no index'th match. */
#define PCI_PROG_IF_ANY 0xFF
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint32_t index, pci_device_t *out);

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

/* ---- M107: memory BARs, and why they needed their own function -------
 *
 * The two functions above return a uint16_t because an I/O BAR encodes a
 * 16-bit port number. A memory BAR is a physical address up to 64 bits
 * wide, spread across two consecutive BAR registers when bit 2 of the low
 * one says so - which is the normal case for these three controllers,
 * because firmware puts them above 4 GiB whenever it feels like it.
 *
 * Returns the physical base with the flag bits masked off, or 0 if BAR
 * `index` is an I/O BAR, is unimplemented, or is the upper half of the
 * 64-bit pair below it (which is not separately addressable and is a
 * caller's bug worth reporting as "no BAR" rather than as a plausible
 * address). Physical address 0 is not a BAR on any machine, so 0 is a
 * safe sentinel here for the same reason port 0 is above. */
uint64_t pci_bar_mem_base(const pci_device_t *dev, uint8_t index);

/* The size of that region, decoded the way the specification says: write
 * all ones, read back which bits stuck, put the original value back.
 * Interrupts are not disabled around it - this runs once per device at
 * boot, before any driver is using the BAR it is sizing.
 *
 * A driver needs this because it must not map more than the device
 * decodes: an xHCI BAR is 64 KiB on QEMU and 512 KiB on a real Intel
 * controller, and mapping a fixed guess would either fault on a register
 * that is there or map a page that is not. */
uint64_t pci_bar_mem_size(const pci_device_t *dev, uint8_t index);

/* ---- The capability list --------------------------------------------
 *
 * Returns the config-space offset of the first capability with this id,
 * or 0 if the device has none (0 is the device-id register, so it is
 * never a capability offset). Bounded at 48 hops and ignores an offset
 * that is not 4-byte aligned or that points into the first 64 bytes:
 * a device whose capability list loops is a device this kernel must not
 * hang on, and firmware-corrupted lists are one of the things M110 is
 * expecting to find. */
#define PCI_CAP_ID_MSI  0x05
#define PCI_CAP_ID_MSIX 0x11
uint8_t pci_find_capability(const pci_device_t *dev, uint8_t cap_id);

/* M107: raw config access, because three drivers now need registers this
 * header has no opinion about - AHCI's ports-implemented mask lives in
 * its BAR, but NVMe's version register and xHCI's legacy-support
 * capability are read straight out of config space. */
uint32_t pci_config_read32(const pci_device_t *dev, uint8_t offset);
void pci_config_write32(const pci_device_t *dev, uint8_t offset, uint32_t value);
