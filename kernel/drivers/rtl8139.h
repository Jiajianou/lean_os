/* kernel/drivers/rtl8139.h
 *
 * RTL8139 Fast Ethernet NIC driver - QEMU's `-device rtl8139` (and real
 * RTL8139-family hardware). Chosen over e1000/virtio-net for the
 * networking stretch goal because its register interface is the smallest
 * of the three: one I/O-mapped BAR, no descriptor rings to build (transmit
 * is 4 fixed physical-address/length register pairs, receive is one
 * contiguous ring buffer the NIC DMAs into and the driver reads out of
 * directly) - the same "smallest thing that's still real hardware" reasoning
 * that picked ATA PIO over AHCI/NVMe back in M12.
 */
#pragma once

#include <stdint.h>

/* Finds the NIC via PCI (pci.h), resets it, allocates its DMA buffers,
 * and brings RX/TX up. Panics if no RTL8139 is attached - matches every
 * other driver's init in this kernel (ata_read_sectors et al assume their
 * hardware exists rather than probing and degrading). Received frames
 * are handed to eth_receive (kernel/net/ethernet.h) directly from the
 * IRQ handler. */
void rtl8139_init(void);

/* This NIC's burned-in MAC address (IDR0-5), read once at init. */
const uint8_t *rtl8139_mac(void);

/* Transmits one Ethernet frame (dst MAC + src MAC + ethertype + payload,
 * already assembled by ethernet.c). Blocks (bounded poll, panics on
 * timeout - same idiom as ata.c's ATA_POLL_LIMIT) until the NIC's DMA
 * engine has actually copied the frame out of the given buffer, so the
 * caller's buffer is safe to reuse/free the instant this returns. len
 * must be <= RTL8139_MAX_FRAME. */
#define RTL8139_MAX_FRAME 1514 /* 14-byte Ethernet header + 1500-byte MTU, no 802.1Q tag */
void rtl8139_send(const uint8_t *frame, uint16_t len);
