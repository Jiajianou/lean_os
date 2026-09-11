#include "rtl8139.h"
#include "rtl8139_ring.h" /* M116 */
#include "dev/random.h" /* M100 */

#include "arch/x86_64/io.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "klog.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "net/ethernet.h"
#include "panic.h"
#include "pci.h"

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

/* Register offsets from the I/O-mapped BAR0 (RTL8139 programmer's guide). */
#define REG_IDR0    0x00 /* MAC address, 6 bytes */
#define REG_TSD0    0x10 /* Transmit Status of Descriptor 0-3, 4 bytes each */
#define REG_TSAD0   0x20 /* Transmit Start Address of Descriptor 0-3, 4 bytes each */
#define REG_RBSTART 0x30 /* Receive Buffer Start Address, 32-bit physical */
#define REG_CMD     0x37
#define REG_CAPR    0x38 /* Current Address of Packet Read (RX ring, 16-bit) */
#define REG_IMR     0x3C
#define REG_ISR     0x3E
#define REG_TCR     0x40
#define REG_RCR     0x44
#define REG_CONFIG1 0x52

#define CMD_TE   (1 << 2) /* transmitter enable */
#define CMD_RE   (1 << 3) /* receiver enable */
#define CMD_RST  (1 << 4)
#define CMD_BUFE (1 << 0) /* RX buffer empty */

#define ISR_ROK (1 << 0) /* receive OK */
#define ISR_TOK (1 << 2) /* transmit OK */

#define RCR_AAP  (1 << 0) /* accept all packets */
#define RCR_APM  (1 << 1) /* accept physical match */
#define RCR_AM   (1 << 2) /* accept multicast */
#define RCR_AB   (1 << 3) /* accept broadcast */
#define RCR_WRAP (1 << 7) /* let a packet straddle the end of the ring into the 1.5K overflow pad */

#define TSD_OWN (1u << 13) /* clear = NIC owns the buffer (still sending); set by hardware once done */

/* RX ring: 8 KiB ring + the overflow pad the RCR_WRAP bit above makes a
 * trailing packet spill into, all physically contiguous (the NIC DMAs
 * into it by physical base address alone, no scatter list) and rounded
 * up to whole 4 KiB frames. The layout is rtl8139_ring.h's business. */
#define RX_BUFFER_SIZE (RTL8139_RING_LEN + RTL8139_RING_PAD)
#define RX_BUFFER_FRAMES ((RX_BUFFER_SIZE + 4095) / 4096)

/* One fixed physical buffer per TX descriptor (4 total), each large
 * enough for the biggest frame ethernet.c ever hands us - avoids needing
 * a general DMA allocator for the common case of "send one frame, wait,
 * reuse the same slot" this driver's synchronous rtl8139_send restricts
 * itself to. */
#define TX_DESCRIPTORS 4

static uint16_t io_base;
static uint8_t mac[6];
static uint8_t *rx_buffer; /* identity-mapped low memory: physical address == this pointer, see pmm.h */
static uint32_t rx_read_offset;
static uint8_t *tx_buffer[TX_DESCRIPTORS];
static int tx_next_descriptor;

#define TX_POLL_LIMIT 1000000 /* generous bound so a wedged NIC panics instead of hanging boot forever - same idiom as ata.c's ATA_POLL_LIMIT */

static void rtl8139_irq(isr_regs_t *regs) {
    (void)regs;
    uint16_t status = inw(io_base + REG_ISR);
    outw(io_base + REG_ISR, status); /* write-1-to-clear, ack whatever we're about to handle */

    if (status & ISR_ROK) {
        while (!(inb(io_base + REG_CMD) & CMD_BUFE)) {
            /* M116: the frame is contiguous even when it runs past the
             * end of the ring - RCR_WRAP puts the tail in the pad rather
             * than at the start. This used to reassemble it from the
             * start of the ring, which is the layout with RCR_WRAP clear,
             * and every frame that straddled the end reached TCP with a
             * stale tail and was discarded by its checksum. */
            rtl8139_rx_t rx;
            rx_read_offset = rtl8139_ring_take(rx_buffer, rx_read_offset, &rx);
            if (rx.len) {
                eth_receive(rx.frame, rx.len);
            }

            /* CAPR quirk: the NIC keeps a 16-byte read-ahead margin, so
             * the value written back is the new offset minus 16 (wrapping
             * mod 0x10000, per every RTL8139 driver reference - not a
             * mistake). */
            outw(io_base + REG_CAPR, (uint16_t)(rx_read_offset - 16));
        }
    }
}

/* Q16: transmits this driver has failed since boot. See ata_error_count
 * for why an error that propagates still needs a counter: it is
 * invisible from outside the caller that saw it. */
static uint32_t tx_errors;

uint32_t rtl8139_tx_error_count(void) {
    return tx_errors;
}

/* Q16: 0 if the reset never completed.
 *
 * This used to halt the machine, and halting is the wrong answer for a
 * card that will not come up: a desktop with no network is a desktop,
 * and a desktop that will not boot because the NIC is sulking is not.
 * The init path below already knows how to report "no card" - a machine
 * with no RTL8139 at all takes that path on every real laptop - so a
 * card that fails its reset takes the same one. */
static int rtl8139_reset(void) {
    outb(io_base + REG_CMD, CMD_RST);
    for (int i = 0; i < TX_POLL_LIMIT; i++) {
        if (!(inb(io_base + REG_CMD) & CMD_RST)) {
            return 1;
        }
    }
    klog_puts("[rtl8139] reset did not complete - carrying on without a network.\n");
    return 0;
}

int rtl8139_init(void) {
    pci_device_t dev;
    if (!pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &dev)) {
        return 0;
    }
    pci_enable_device(&dev);
    io_base = pci_bar0_io_base(&dev);
    if (io_base == 0) {
        klog_puts("[rtl8139] BAR0 is memory-mapped - this driver speaks port I/O only.\n");
        return 0; /* Q16 - see pci_bar0_io_base */
    }

    outb(io_base + REG_CONFIG1, 0x00); /* power on */
    if (!rtl8139_reset()) {
        return 0; /* Q16: a card that will not reset is a machine with no network, not a dead machine */
    }

    for (int i = 0; i < 6; i++) {
        mac[i] = inb(io_base + REG_IDR0 + i);
    }

    uint64_t rx_phys = pmm_alloc_contiguous(RX_BUFFER_FRAMES);
    rx_buffer = (uint8_t *)(uintptr_t)rx_phys;
    rx_read_offset = 0;
    outl(io_base + REG_RBSTART, (uint32_t)rx_phys);

    for (int i = 0; i < TX_DESCRIPTORS; i++) {
        uint64_t tx_phys = pmm_alloc_contiguous(1);
        tx_buffer[i] = (uint8_t *)(uintptr_t)tx_phys;
    }
    tx_next_descriptor = 0;

    outl(io_base + REG_TCR, 0); /* default transmit config is fine - no loopback, standard IFG */
    outl(io_base + REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);

    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);
    outb(io_base + REG_CMD, CMD_RE | CMD_TE);

    irq_register_handler(dev.irq_line, rtl8139_irq);
    irq_enable_line(dev.irq_line);

    /* M100: the MAC is not entropy, but it is different on every machine
     * that is not this QEMU, and a seed that is at least distinct
     * everywhere is better than one that is the same. */
    random_feed(mac, sizeof(mac));
    klog_puts("[net] rtl8139 found at PCI ");
    klog_put_hex32(dev.bus);
    klog_puts(":");
    klog_put_hex32(dev.slot);
    klog_puts(":");
    klog_put_hex32(dev.func);
    klog_puts(", io_base=0x");
    klog_put_hex32(io_base);
    klog_puts(", irq=");
    klog_put_hex32(dev.irq_line);
    klog_puts(", mac=0x");
    uint64_t mac_packed = 0;
    for (int i = 0; i < 6; i++) {
        mac_packed = (mac_packed << 8) | mac[i];
    }
    klog_put_hex64(mac_packed);
    klog_putc('\n');
    return 1;
}

const uint8_t *rtl8139_mac(void) {
    return mac;
}

int rtl8139_send(const uint8_t *frame, uint16_t len) {
    /* Q16: refused rather than fatal. An over-long frame is a bug in the
     * caller - ethernet.c is the only one - and the honest answer to a
     * caller's bug is to refuse the operation and let it see that,
     * rather than to take the machine down with it. Counted, so a caller
     * that is quietly getting this wrong is visible from outside. */
    if (len > RTL8139_MAX_FRAME) {
        tx_errors++;
        return -1;
    }

    int slot = tx_next_descriptor;
    tx_next_descriptor = (tx_next_descriptor + 1) % TX_DESCRIPTORS;

    /* No "wait until this descriptor is free" poll before reusing it: the
     * poll at the bottom of this function already blocks until the NIC
     * hands ownership of `slot` back before returning, so by the time
     * round-robin cycles back to the same slot it's guaranteed idle - and
     * the OWN bit's power-on-reset value is undefined, so checking it
     * before a descriptor's very first use could spin the full timeout
     * and panic on the first packet ever sent. */
    k_memcpy(tx_buffer[slot], frame, len);
    outl(io_base + REG_TSAD0 + (uint32_t)slot * 4, (uint32_t)(uintptr_t)tx_buffer[slot]);
    /* Writing TSD's length field (bits 0-12) triggers transmission; the
     * Ethernet minimum frame size is 60 bytes + 4-byte CRC the NIC appends
     * itself, so anything ethernet.c hands us shorter than 60 needs
     * padding - callers are expected to have already zero-padded (see
     * ethernet.c's eth_send). */
    outl(io_base + REG_TSD0 + (uint32_t)slot * 4, len);

    for (int i = 0; i < TX_POLL_LIMIT; i++) {
        if (inl(io_base + REG_TSD0 + (uint32_t)slot * 4) & TSD_OWN) {
            return 0;
        }
    }
    /* Q16: a transmit that never drained. On a real link this is a cable
     * pulled or a card that has stopped answering, and neither is a
     * reason to stop the machine - the packet is lost and every protocol
     * above this one is built to survive a lost packet. That is more
     * than can be said for a panic. */
    tx_errors++;
    return -1;
}
