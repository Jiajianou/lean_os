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
 * and brings RX/TX up. Returns 1 if a NIC was found and initialized, 0 if
 * none is attached at all - unlike every other driver in this kernel
 * (ata_read_sectors et al panic if their hardware is missing), because
 * RTL8139 specifically is a legacy chip real machines - the whole point
 * of the "port to real hardware" stretch goal this exists alongside -
 * essentially never actually have, unlike PS/2 or a PC-standard PIT.
 * Only a real fault on hardware that *is* present (a reset that never
 * completes, a stuck TX descriptor) still panics, same as every other
 * driver - see rtl8139_reset/rtl8139_send. Received frames are handed to
 * eth_receive (kernel/net/ethernet.h) directly from the IRQ handler. */
int rtl8139_init(void);

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
